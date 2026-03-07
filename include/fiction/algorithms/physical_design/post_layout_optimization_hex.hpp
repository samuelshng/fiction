/**
 * @file post_layout_optimization_hex.hpp
 * @brief Native post-layout optimization for row-clocked pointy-top hexagonal layouts.
 */

#ifndef FICTION_POST_LAYOUT_OPTIMIZATION_HEX_HPP
#define FICTION_POST_LAYOUT_OPTIMIZATION_HEX_HPP

#include "fiction/algorithms/path_finding/a_star.hpp"
#include "fiction/algorithms/path_finding/cost.hpp"
#include "fiction/algorithms/path_finding/distance.hpp"
#include "fiction/algorithms/physical_design/post_layout_optimization.hpp"
#include "fiction/algorithms/verification/equivalence_checking.hpp"
#include "fiction/layouts/bounding_box.hpp"
#include "fiction/layouts/clocking_scheme.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/routing_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace fiction
{

namespace detail
{

/**
 * @brief Routing objective for fixed-gate native hex rewiring.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 */
template <typename Lyt>
struct hex_routing_objective
{
    /**
     * @brief Source tile.
     */
    coordinate<Lyt> source{};
    /**
     * @brief Target tile.
     */
    coordinate<Lyt> target{};
    /**
     * @brief Source output pin index.
     */
    uint8_t source_output{0u};
    /**
     * @brief Target input pin index.
     */
    uint32_t target_input{0u};
};

/**
 * @brief Returns the occupied 2D area of a layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Occupied layout area.
 */
template <typename Lyt>
[[nodiscard]] uint64_t layout_area(const Lyt& lyt) noexcept
{
    return static_cast<uint64_t>(lyt.x() + 1u) * static_cast<uint64_t>(lyt.y() + 1u);
}

/**
 * @brief Returns the number of internal routing wires of a layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Number of non-I/O wire segments.
 */
template <typename Lyt>
[[nodiscard]] uint64_t internal_wire_count(const Lyt& lyt) noexcept
{
    return lyt.num_wires() - lyt.num_pis() - lyt.num_pos();
}

/**
 * @brief Sorts routing objectives by descending Manhattan distance.
 *
 * Longer objectives are routed first to reduce the chance that short local routes block global connections.
 *
 * @tparam Lyt Gate-level layout type.
 * @param objectives Objectives to sort.
 */
template <typename Lyt>
void sort_hex_routing_objectives(std::vector<hex_routing_objective<Lyt>>& objectives)
{
    std::stable_sort(
        objectives.begin(), objectives.end(),
        [](const auto& lhs, const auto& rhs)
        {
            if (lhs.target == rhs.target && lhs.target_input != rhs.target_input)
            {
                return lhs.target_input < rhs.target_input;
            }

            const auto lhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.source.x) - static_cast<int64_t>(lhs.target.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.source.y) - static_cast<int64_t>(lhs.target.y)));
            const auto rhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.source.x) - static_cast<int64_t>(rhs.target.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.source.y) - static_cast<int64_t>(rhs.target.y)));

            return lhs_distance > rhs_distance;
        });
}

/**
 * @brief Returns whether a node is an intermediate routing element.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param n Node to inspect.
 * @return `true` iff `n` is a routing wire or fanout that can be recreated during rerouting.
 */
template <typename Lyt>
[[nodiscard]] bool is_connection_wire(const Lyt& lyt, const mockturtle::node<Lyt>& n) noexcept
{
    return lyt.is_wire(n) && !lyt.is_pi(n) && !lyt.is_po(n);
}

/**
 * @brief Incoming signal reference between two adjacent layout nodes.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct incoming_signal_reference
{
    /**
     * @brief Signal found in the target fanin list.
     */
    mockturtle::signal<Lyt> signal{};
    /**
     * @brief Position of the signal in the target fanin list.
     */
    uint32_t index{0u};
};

/**
 * @brief Finds the incoming signal by which `source` drives `target`.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param source Driving node.
 * @param target Driven node.
 * @return Matching incoming signal and its target input index if present.
 */
template <typename Lyt>
[[nodiscard]] std::optional<incoming_signal_reference<Lyt>>
find_incoming_signal_from(const Lyt& lyt, const mockturtle::node<Lyt>& source, const mockturtle::node<Lyt>& target)
{
    std::optional<incoming_signal_reference<Lyt>> incoming_signal{};
    uint32_t                                     fanin_index{0u};

    const auto collector = [&lyt, &source, &incoming_signal, &fanin_index](const auto& fin)
    {
        if (lyt.get_node(fin) == source)
        {
            incoming_signal = incoming_signal_reference<Lyt>{fin, fanin_index};
            return false;
        }

        ++fanin_index;
        return true;
    };

    lyt.template foreach_fanin<decltype(collector), false>(target, std::move(collector));

    return incoming_signal;
}

/**
 * @brief Extracts routing objectives including source output pins from a placed layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Routing objectives that can recreate the current connectivity after clearing all routing wires.
 */
template <typename Lyt>
[[nodiscard]] std::vector<hex_routing_objective<Lyt>> extract_hex_routing_objectives(const Lyt& lyt) noexcept
{
    std::vector<hex_routing_objective<Lyt>> objectives{};
    std::vector<bool>                       visited(lyt.size(), false);

    const std::function<void(const coordinate<Lyt>&, uint8_t, uint32_t, const mockturtle::node<Lyt>&)>
        recursively_traverse_paths = [&](const auto& recent_gate_tile, const uint8_t recent_source_output,
                                         const uint32_t recent_target_input, const auto& current_node)
    {
        auto current_gate_tile = recent_gate_tile;

        if (!is_connection_wire(lyt, current_node))
        {
            current_gate_tile = lyt.get_tile(current_node);

            if (recent_gate_tile != current_gate_tile)
            {
                objectives.push_back({recent_gate_tile, current_gate_tile, recent_source_output, recent_target_input});
            }
        }

        if (visited[current_node])
        {
            return;
        }

        visited[current_node] = true;

        lyt.foreach_fanout(
            current_node,
            [&](const auto& fanout_node)
            {
                const auto incoming_signal = find_incoming_signal_from(lyt, current_node, fanout_node);
                const auto source_output =
                    is_connection_wire(lyt, current_node) ?
                        recent_source_output :
                        (incoming_signal.has_value() ? incoming_signal->signal.output : uint8_t{0});
                const auto target_input = incoming_signal.has_value() ? incoming_signal->index : uint32_t{0};
                recursively_traverse_paths(current_gate_tile, source_output, target_input, fanout_node);
            });
    };

    lyt.foreach_pi(
        [&](const auto& pi)
        {
            const auto pi_tile = lyt.get_tile(pi);

            lyt.foreach_fanout(
                pi,
                [&](const auto& fanout_node)
                {
                    const auto incoming_signal = find_incoming_signal_from(lyt, pi, fanout_node);
                    const auto source_output =
                        incoming_signal.has_value() ? incoming_signal->signal.output : uint8_t{0};
                    const auto target_input = incoming_signal.has_value() ? incoming_signal->index : uint32_t{0};
                    recursively_traverse_paths(pi_tile, source_output, target_input, fanout_node);
                });
        });

    sort_hex_routing_objectives(objectives);

    return objectives;
}

/**
 * @brief Shrinks a layout to its occupied 2D bounding box.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to compact.
 */
template <typename Lyt>
void compact_to_bounding_box(Lyt& lyt)
{
    const auto bounding_box = bounding_box_2d(lyt);
    lyt.resize({bounding_box.get_max().x, bounding_box.get_max().y, lyt.z()});
}

/**
 * @brief Removes all recreatable routing from a native hex layout.
 *
 * Unlike the generic routing clear helper, this variant also removes fanout buffers because native hex compaction is
 * free to rebuild signal branching at different locations.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout whose routing is to be deleted.
 */
template <typename Lyt>
void clear_hex_routing(Lyt& lyt) noexcept
{
    lyt.foreach_node(
        [&lyt](const auto& g)
        {
            if (lyt.is_constant(g))
            {
                return;
            }

            const auto t = lyt.get_tile(g);

            if (lyt.is_wire(g) && !lyt.is_pi(g) && !lyt.is_po(g))
            {
                lyt.clear_tile(t);
            }
            else
            {
                lyt.move_node(g, t);
            }
        });
}

/**
 * @brief Shifts a coordinate one row upward if it lies below a removed row.
 *
 * @tparam Lyt Gate-level layout type.
 * @param c Coordinate to adjust.
 * @param row Removed row.
 * @return Adjusted coordinate.
 */
template <typename Lyt>
[[nodiscard]] coordinate<Lyt> shift_coordinate_after_row_removal(coordinate<Lyt> c, const uint64_t row) noexcept
{
    if (c.y > row)
    {
        --c.y;
    }

    return c;
}

/**
 * @brief Returns whether a given row contains only removable routing wires.
 *
 * Empty rows are considered removable as well. Fanouts, gates, and I/O tiles block row removal.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param row Row to inspect.
 * @return `true` iff the row can be removed safely before rerouting.
 */
template <typename Lyt>
[[nodiscard]] bool is_removable_hex_row(const Lyt& lyt, const uint64_t row) noexcept
{
    for (uint64_t x = 0u; x <= lyt.x(); ++x)
    {
        for (uint64_t z = 0u; z <= lyt.z(); ++z)
        {
            const auto t = tile<Lyt>{x, row, z};

            if (lyt.is_empty_tile(t))
            {
                continue;
            }

            if (!is_connection_wire(lyt, lyt.get_node(t)))
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief Adapts routing objectives after removing one horizontal row.
 *
 * @tparam Lyt Gate-level layout type.
 * @param objectives Original routing objectives.
 * @param row Removed row.
 * @return Objectives with shifted source/target coordinates.
 */
template <typename Lyt>
[[nodiscard]] std::vector<hex_routing_objective<Lyt>>
shift_objectives_after_row_removal(const std::vector<hex_routing_objective<Lyt>>& objectives, const uint64_t row)
{
    auto shifted_objectives = objectives;

    for (auto& objective : shifted_objectives)
    {
        objective.source = shift_coordinate_after_row_removal<Lyt>(objective.source, row);
        objective.target = shift_coordinate_after_row_removal<Lyt>(objective.target, row);
    }

    sort_hex_routing_objectives(shifted_objectives);

    return shifted_objectives;
}

/**
 * @brief Moves every structural node below a removed row one step upward.
 *
 * Routing wires are expected to be cleared before this helper is called.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to update.
 * @param row Removed row.
 */
template <typename Lyt>
void shift_structural_nodes_up_after_row_removal(Lyt& lyt, const uint64_t row)
{
    std::vector<std::pair<tile<Lyt>, mockturtle::node<Lyt>>> nodes_to_shift{};

    lyt.foreach_node(
        [&lyt, row, &nodes_to_shift](const auto& n)
        {
            if (lyt.is_constant(n))
            {
                return;
            }

            const auto node_tile = lyt.get_tile(n);

            if (node_tile.is_dead() || node_tile.y <= row)
            {
                return;
            }

            nodes_to_shift.emplace_back(node_tile, n);
        });

    std::sort(
        nodes_to_shift.begin(), nodes_to_shift.end(),
        [](const auto& lhs, const auto& rhs)
        {
            if (lhs.first.y != rhs.first.y)
            {
                return lhs.first.y < rhs.first.y;
            }

            if (lhs.first.z != rhs.first.z)
            {
                return lhs.first.z < rhs.first.z;
            }

            return lhs.first.x < rhs.first.x;
        });

    for (const auto& [old_tile, node] : nodes_to_shift)
    {
        auto new_tile = old_tile;
        --new_tile.y;
        lyt.move_node(node, new_tile);
    }

    lyt.resize({lyt.x(), lyt.y() - 1u, lyt.z()});
}

/**
 * @brief Re-routes a fixed native hex layout without moving gates.
 *
 * All gates remain at their current coordinates. Only routing wires are removed and recreated through shortest paths
 * with crossings disabled.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to reroute.
 * @param routing_objectives Routing objectives to reconstruct.
 * @return `true` iff all routing objectives could be reconstructed.
 */
template <typename Lyt>
[[nodiscard]] bool reroute_hex_layout_wires(Lyt& lyt, const std::vector<hex_routing_objective<Lyt>>& routing_objectives)
{
    if (routing_objectives.empty())
    {
        return true;
    }

    const auto original_layout = lyt.clone();

    clear_hex_routing(lyt);

    a_star_params params{};
    params.crossings = false;

    for (const auto& objective : routing_objectives)
    {
        if (objective.source == objective.target)
        {
            continue;
        }

        const auto path = a_star<layout_coordinate_path<Lyt>>(lyt, {objective.source, objective.target},
                                                              euclidean_distance_functor<Lyt>(), unit_cost_functor<Lyt>(),
                                                              params);

        if (path.empty())
        {
            lyt = original_layout;
            return false;
        }

        auto incoming_signal = lyt.make_signal(lyt.get_node(objective.source), objective.source_output);

        std::for_each(
            path.cbegin() + 1, path.cend() - 1,
            [&lyt, &incoming_signal](const auto& coord)
            { incoming_signal = lyt.create_buf(incoming_signal, lyt.is_empty_tile(coord) ? coord : lyt.above(coord)); });

        const auto target_node = lyt.get_node(path.target());
        auto       target_tile = lyt.get_tile(target_node);

        std::vector<mockturtle::signal<Lyt>> target_children{};
        lyt.foreach_fanin(target_node, [&target_children](const auto& fin) { target_children.push_back(fin); });

        if (objective.target_input > target_children.size())
        {
            lyt = original_layout;
            return false;
        }

        target_children.insert(target_children.cbegin() + static_cast<int64_t>(objective.target_input), incoming_signal);
        lyt.move_node(target_node, target_tile, target_children);
    }

    return true;
}

/**
 * @brief Re-routes a fixed native hex layout without moving gates.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to reroute.
 * @return `true` iff all routing objectives could be reconstructed.
 */
template <typename Lyt>
[[nodiscard]] bool reroute_hex_layout_wires(Lyt& lyt)
{
    return reroute_hex_layout_wires(lyt, extract_hex_routing_objectives(lyt));
}

/**
 * @brief Removes vertically redundant wire-only rows from a native hex layout.
 *
 * Each candidate row is removed by shifting all structural nodes below it upward by one row and re-routing all
 * connections. The candidate is accepted only if it stays equivalent and does not worsen wire count.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to compact.
 * @return `true` iff at least one row could be removed.
 */
template <typename Lyt>
[[nodiscard]] bool compact_hex_wire_rows(Lyt& lyt)
{
    auto improved = false;
    auto progress = true;

    while (progress && lyt.y() > 0u)
    {
        progress = false;

        for (uint64_t row = 1u; row < lyt.y(); ++row)
        {
            if (!is_removable_hex_row(lyt, row))
            {
                continue;
            }

            auto candidate          = lyt.clone();
            const auto objectives   = shift_objectives_after_row_removal(extract_hex_routing_objectives(candidate), row);
            const auto current_area = layout_area(lyt);
            const auto current_wires = internal_wire_count(lyt);

            clear_hex_routing(candidate);
            shift_structural_nodes_up_after_row_removal(candidate, row);

            if (!reroute_hex_layout_wires(candidate, objectives))
            {
                continue;
            }

            compact_to_bounding_box(candidate);

            if (equivalence_checking(lyt, candidate) == eq_type::NO)
            {
                continue;
            }

            const auto candidate_area  = layout_area(candidate);
            const auto candidate_wires = internal_wire_count(candidate);
            const auto improves_layout =
                (candidate_wires < current_wires) || (candidate_wires == current_wires && candidate_area < current_area);

            if (!improves_layout)
            {
                continue;
            }

            lyt      = std::move(candidate);
            improved = true;
            progress = true;
            break;
        }
    }

    return improved;
}

}  // namespace detail

/**
 * @brief Post-layout optimization for native row-clocked pointy-top hexagonal gate-level layouts.
 *
 * Native hexagonal layouts are optimized in three conservative steps:
 * 1. compact the occupied bounding box,
 * 2. remove vertically redundant rows that contain routing wires only,
 * 3. re-route existing connections with gates fixed in place to reduce excess wire detours.
 *
 * The rerouted layout is kept only if it does not worsen area or wiring.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Row-clocked pointy-top hexagonal layout to optimize.
 * @param ps Post-layout optimization parameters.
 * @param pst Optional optimization statistics.
 */
template <typename Lyt>
void post_layout_optimization_hex(const Lyt& lyt, [[maybe_unused]] post_layout_optimization_params ps = {},
                                  post_layout_optimization_stats* pst = nullptr) noexcept
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    post_layout_optimization_stats st{};
    {
        const mockturtle::stopwatch stop{st.time_total};

        st.x_size_before        = lyt.x() + 1;
        st.y_size_before        = lyt.y() + 1;
        st.num_wires_before     = lyt.num_wires() - lyt.num_pis() - lyt.num_pos();
        st.num_crossings_before = lyt.num_crossings();

        auto& mutable_layout = const_cast<Lyt&>(lyt);

        if (!lyt.is_clocking_scheme(clock_name::ROW))
        {
            std::cout << "[e] the given hexagonal layout has to be ROW-clocked\n";

            st.x_size_after        = st.x_size_before;
            st.y_size_after        = st.y_size_before;
            st.num_wires_after     = st.num_wires_before;
            st.num_crossings_after = st.num_crossings_before;

            if (pst != nullptr)
            {
                *pst = st;
            }

            return;
        }

        auto best_layout = lyt.clone();
        detail::compact_to_bounding_box(best_layout);
        [[maybe_unused]] const auto removed_hex_rows = detail::compact_hex_wire_rows(best_layout);
        detail::compact_to_bounding_box(best_layout);

        auto rerouted_layout = best_layout.clone();

        if (detail::reroute_hex_layout_wires(rerouted_layout))
        {
            detail::compact_to_bounding_box(rerouted_layout);

            const auto best_area          = detail::layout_area(best_layout);
            const auto rerouted_area      = detail::layout_area(rerouted_layout);
            const auto best_wire_count    = detail::internal_wire_count(best_layout);
            const auto rerouted_wire_count = detail::internal_wire_count(rerouted_layout);

            const auto equivalent_layouts = equivalence_checking(best_layout, rerouted_layout) != eq_type::NO;
            const auto improves_layout =
                (rerouted_wire_count < best_wire_count) ||
                (rerouted_wire_count == best_wire_count && rerouted_area <= best_area);

            if (equivalent_layouts && improves_layout)
            {
                best_layout = std::move(rerouted_layout);
            }
            else if (!equivalent_layouts && improves_layout)
            {
                std::cout << "[w] discarded native hex rewiring candidate because it changed functionality\n";
            }
        }

        mutable_layout = best_layout;

        st.x_size_after        = mutable_layout.x() + 1;
        st.y_size_after        = mutable_layout.y() + 1;
        st.num_wires_after     = mutable_layout.num_wires() - mutable_layout.num_pis() - mutable_layout.num_pos();
        st.num_crossings_after = mutable_layout.num_crossings();

        const auto area_before = static_cast<uint64_t>(st.x_size_before) * static_cast<uint64_t>(st.y_size_before);
        const auto area_after  = static_cast<uint64_t>(st.x_size_after) * static_cast<uint64_t>(st.y_size_after);

        const auto area_difference = static_cast<double>(area_before) - static_cast<double>(area_after);
        st.area_improvement =
            std::round((area_difference / static_cast<double>(std::max<uint64_t>(1u, area_before))) * 10000.0) / 100.0;
    }

    if (pst != nullptr)
    {
        *pst = st;
    }
}

}  // namespace fiction

#endif  // FICTION_POST_LAYOUT_OPTIMIZATION_HEX_HPP
