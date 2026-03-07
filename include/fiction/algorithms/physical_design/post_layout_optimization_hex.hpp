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
};

/**
 * @brief Returns whether a node is an intermediate routing wire.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param n Node to inspect.
 * @return `true` iff `n` is a non-branching routing wire.
 */
template <typename Lyt>
[[nodiscard]] bool is_connection_wire(const Lyt& lyt, const mockturtle::node<Lyt>& n) noexcept
{
    return lyt.is_wire(n) && !lyt.is_fanout(n) && !lyt.is_pi(n) && !lyt.is_po(n);
}

/**
 * @brief Finds the incoming signal by which `source` drives `target`.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param source Driving node.
 * @param target Driven node.
 * @return Matching incoming signal if present.
 */
template <typename Lyt>
[[nodiscard]] std::optional<mockturtle::signal<Lyt>> find_incoming_signal_from(const Lyt& lyt,
                                                                               const mockturtle::node<Lyt>& source,
                                                                               const mockturtle::node<Lyt>& target)
{
    std::optional<mockturtle::signal<Lyt>> incoming_signal{};

    const auto collector = [&lyt, &source, &incoming_signal](const auto& fin)
    {
        if (lyt.get_node(fin) == source)
        {
            incoming_signal = fin;
            return false;
        }

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

    const std::function<void(const coordinate<Lyt>&, uint8_t, const mockturtle::node<Lyt>&)> recursively_traverse_paths =
        [&](const auto& recent_gate_tile, const uint8_t recent_source_output, const auto& current_node)
    {
        auto current_gate_tile = recent_gate_tile;

        if (!is_connection_wire(lyt, current_node))
        {
            current_gate_tile = lyt.get_tile(current_node);

            if (recent_gate_tile != current_gate_tile)
            {
                objectives.push_back({recent_gate_tile, current_gate_tile, recent_source_output});
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
                const auto source_output   = incoming_signal.has_value() ? incoming_signal->output : uint8_t{0};
                recursively_traverse_paths(current_gate_tile, source_output, fanout_node);
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
                    const auto source_output   = incoming_signal.has_value() ? incoming_signal->output : uint8_t{0};
                    recursively_traverse_paths(pi_tile, source_output, fanout_node);
                });
        });

    std::stable_sort(
        objectives.begin(), objectives.end(),
        [](const auto& lhs, const auto& rhs)
        {
            const auto lhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.source.x) - static_cast<int64_t>(lhs.target.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.source.y) - static_cast<int64_t>(lhs.target.y)));
            const auto rhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.source.x) - static_cast<int64_t>(rhs.target.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.source.y) - static_cast<int64_t>(rhs.target.y)));

            return lhs_distance > rhs_distance;
        });

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
 * @brief Re-routes a fixed native hex layout without moving gates.
 *
 * All gates remain at their current coordinates. Only routing wires are removed and recreated through shortest paths
 * with crossings disabled.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to reroute.
 * @return `true` iff all routing objectives could be reconstructed.
 */
template <typename Lyt>
[[nodiscard]] bool reroute_hex_layout_wires(Lyt& lyt)
{
    const auto routing_objectives = extract_hex_routing_objectives(lyt);

    if (routing_objectives.empty())
    {
        return true;
    }

    const auto original_layout = lyt.clone();

    clear_routing(lyt);

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

        route_path(lyt, lyt.make_signal(lyt.get_node(objective.source), objective.source_output), path);
    }

    return true;
}

}  // namespace detail

/**
 * @brief Post-layout optimization for native row-clocked pointy-top hexagonal gate-level layouts.
 *
 * Native hexagonal layouts are optimized in two conservative steps:
 * 1. compact the occupied bounding box,
 * 2. re-route existing connections with gates fixed in place to reduce excess wire detours.
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

        auto rerouted_layout = best_layout.clone();

        if (detail::reroute_hex_layout_wires(rerouted_layout))
        {
            detail::compact_to_bounding_box(rerouted_layout);

            const auto best_area =
                static_cast<uint64_t>(best_layout.x() + 1u) * static_cast<uint64_t>(best_layout.y() + 1u);
            const auto rerouted_area =
                static_cast<uint64_t>(rerouted_layout.x() + 1u) * static_cast<uint64_t>(rerouted_layout.y() + 1u);

            const auto best_wire_count =
                best_layout.num_wires() - best_layout.num_pis() - best_layout.num_pos();
            const auto rerouted_wire_count =
                rerouted_layout.num_wires() - rerouted_layout.num_pis() - rerouted_layout.num_pos();

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
