/**
 * @file post_layout_optimization_hex.hpp
 * @brief Native post-layout optimization for row-clocked pointy-top hexagonal layouts.
 */

#ifndef FICTION_POST_LAYOUT_OPTIMIZATION_HEX_HPP
#define FICTION_POST_LAYOUT_OPTIMIZATION_HEX_HPP

#include "fiction/algorithms/path_finding/a_star.hpp"
#include "fiction/algorithms/path_finding/cost.hpp"
#include "fiction/algorithms/path_finding/distance.hpp"
#include "fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp"
#include "fiction/algorithms/physical_design/post_layout_optimization.hpp"
#include "fiction/algorithms/verification/equivalence_checking.hpp"
#include "fiction/layouts/bounding_box.hpp"
#include "fiction/layouts/clocking_scheme.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/name_utils.hpp"
#include "fiction/utils/routing_utils.hpp"

#include <mockturtle/algorithms/cleanup.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
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
 * @brief Routing metadata for one primary output.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct hex_po_routing_data
{
    /**
     * @brief Index of the corresponding routing objective inside the extracted objective list.
     */
    std::size_t objective_index{0u};
    /**
     * @brief Current PO tile.
     */
    coordinate<Lyt> current_target{};
    /**
     * @brief Preferred x coordinate for re-placement.
     */
    uint64_t preferred_x{0u};
};

/**
 * @brief One fan-in connection of a locally relocated gate.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct hex_local_fanin
{
    /**
     * @brief Upstream source signal that remains fixed while the gate is moved.
     */
    mockturtle::signal<Lyt> source_signal{};
    /**
     * @brief Input position of the moved gate that this signal feeds.
     */
    uint32_t gate_input{0u};
};

/**
 * @brief One fan-out connection of a locally relocated gate.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct hex_local_fanout
{
    /**
     * @brief Downstream target tile that remains fixed while the gate is moved.
     */
    coordinate<Lyt> target{};
    /**
     * @brief Output pin index of the moved gate that drives the target.
     */
    uint8_t source_output{0u};
    /**
     * @brief Input position at the target that must be preserved.
     */
    uint32_t target_input{0u};
};

/**
 * @brief Local routing neighborhood of one movable structural gate.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct hex_local_relocation_data
{
    /**
     * @brief Fan-in connections that must be re-established after relocation.
     */
    std::vector<hex_local_fanin<Lyt>> fanins{};
    /**
     * @brief Fan-out connections that must be re-established after relocation.
     */
    std::vector<hex_local_fanout<Lyt>> fanouts{};
    /**
     * @brief Rebuildable local routing wires that may be deleted to free up space.
     */
    std::vector<tile<Lyt>> to_clear{};
};

/**
 * @brief Shrinks a layout to its occupied 2D bounding box.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to compact.
 */
template <typename Lyt>
void compact_to_bounding_box(Lyt& lyt);

/**
 * @brief Extracts routing objectives including source output pins from a placed layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Routing objectives that can recreate the current connectivity after clearing all routing wires.
 */
template <typename Lyt>
[[nodiscard]] std::vector<hex_routing_objective<Lyt>> extract_hex_routing_objectives(const Lyt& lyt) noexcept;

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
    std::stable_sort(objectives.begin(), objectives.end(),
                     [](const auto& lhs, const auto& rhs)
                     {
                         if (lhs.target == rhs.target && lhs.target_input != rhs.target_input)
                         {
                             return lhs.target_input < rhs.target_input;
                         }

                         const auto lhs_distance = static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.source.x) -
                                                                                  static_cast<int64_t>(lhs.target.x))) +
                                                   static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.source.y) -
                                                                                  static_cast<int64_t>(lhs.target.y)));
                         const auto rhs_distance = static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.source.x) -
                                                                                  static_cast<int64_t>(rhs.target.x))) +
                                                   static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.source.y) -
                                                                                  static_cast<int64_t>(rhs.target.y)));

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
    uint32_t                                      fanin_index{0u};

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
 * @brief Traces a routed layout signal back to its driving structural source.
 *
 * Routing wires and fanout buffers are skipped until a PI, constant, or structural logic gate is reached. The
 * returned signal preserves the output pin of the structural source.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param routed_signal Signal somewhere inside a routed connection.
 * @return Signal emitted by the structural source of that connection.
 */
template <typename Lyt>
[[nodiscard]] mockturtle::signal<Lyt> trace_hex_structural_signal(const Lyt&              lyt,
                                                                  mockturtle::signal<Lyt> routed_signal) noexcept
{
    auto current_signal = routed_signal;
    auto current_node   = lyt.get_node(current_signal);

    if (lyt.is_po(current_node))
    {
        std::vector<mockturtle::signal<Lyt>> po_fanins{};
        const auto po_fanin_collector = [&po_fanins](const auto& fin) { po_fanins.push_back(fin); };
        lyt.template foreach_fanin<decltype(po_fanin_collector), false>(current_node, std::move(po_fanin_collector));

        if (!po_fanins.empty())
        {
            current_signal = po_fanins.front();
            current_node   = lyt.get_node(current_signal);
        }
    }

    while (is_connection_wire(lyt, current_node))
    {
        std::vector<mockturtle::signal<Lyt>> fanins{};
        const auto                           fanin_collector = [&fanins](const auto& fin) { fanins.push_back(fin); };
        lyt.template foreach_fanin<decltype(fanin_collector), false>(current_node, std::move(fanin_collector));

        if (fanins.empty())
        {
            break;
        }

        current_signal = fanins.front();
        current_node   = lyt.get_node(current_signal);
    }

    return current_signal;
}

/**
 * @brief Converts one traced layout signal into the corresponding technology-network signal.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Source layout.
 * @param old2new Mapping from layout nodes to technology-network signals.
 * @param traced_signal Structural source signal in the layout.
 * @return Equivalent signal in the technology network.
 */
template <typename Lyt>
[[nodiscard]] mockturtle::signal<tec_nt>
convert_hex_signal_to_technology_signal(const Lyt& lyt, tec_nt& ntk,
                                        mockturtle::node_map<std::vector<mockturtle::signal<tec_nt>>, Lyt>& old2new,
                                        const mockturtle::signal<Lyt>& traced_signal)
{
    const auto source_node    = lyt.get_node(traced_signal);
    auto&      mapped_outputs = old2new[source_node];
    const auto output_pin     = static_cast<std::size_t>(signal_output_pin<Lyt>(traced_signal));

    if (mapped_outputs.empty())
    {
        if (lyt.is_constant(source_node))
        {
            mapped_outputs = {ntk.get_constant(lyt.constant_value(source_node))};
        }
        else if (lyt.is_pi(source_node))
        {
            throw std::runtime_error{
                "native hex GOLD fallback encountered unmapped PI source node " + std::to_string(source_node) +
                " at tile (" + std::to_string(lyt.get_tile(source_node).x) + ", " +
                std::to_string(lyt.get_tile(source_node).y) + ", " + std::to_string(lyt.get_tile(source_node).z) + ")"};
        }
    }

    if (mapped_outputs.empty())
    {
        throw std::runtime_error{
            "native hex GOLD fallback could not map traced source node " + std::to_string(source_node) + " at tile (" +
            std::to_string(lyt.get_tile(source_node).x) + ", " + std::to_string(lyt.get_tile(source_node).y) + ", " +
            std::to_string(lyt.get_tile(source_node).z) + ")"};
    }

    if (mapped_outputs.size() == 1u)
    {
        return mapped_outputs.front();
    }

    if (output_pin >= mapped_outputs.size())
    {
        throw std::runtime_error{"native hex GOLD fallback encountered invalid structural output pin " +
                                 std::to_string(output_pin) + " for source node " + std::to_string(source_node)};
    }

    return mapped_outputs[output_pin];
}

/**
 * @brief Returns the number of non-routing structural gates in a native hex layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Number of movable structural logic gates.
 */
template <typename Lyt>
[[nodiscard]] uint64_t count_hex_structural_gates(const Lyt& lyt) noexcept
{
    uint64_t count{0u};

    lyt.foreach_gate(
        [&lyt, &count](const auto& gate)
        {
            if (!is_connection_wire(lyt, gate))
            {
                ++count;
            }
        });

    return count;
}

/**
 * @brief Ensures that one structural layout node has been reconstructed in the extracted technology network.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Source layout.
 * @param ntk Extracted technology network.
 * @param old2new Mapping from layout nodes to extracted output signals.
 * @param node Structural source node to reconstruct.
 */
template <typename Lyt>
void ensure_mapped_hex_structural_node(const Lyt& lyt, tec_nt& ntk,
                                       mockturtle::node_map<std::vector<mockturtle::signal<tec_nt>>, Lyt>& old2new,
                                       std::vector<uint8_t>& visit_state, const mockturtle::node<Lyt>& node)
{
    if (!old2new[node].empty() || lyt.is_constant(node) || lyt.is_pi(node))
    {
        return;
    }

    if (lyt.is_po(node) || is_connection_wire(lyt, node))
    {
        throw std::runtime_error{"native hex GOLD fallback encountered non-structural source node " +
                                 std::to_string(node) + " at tile (" + std::to_string(lyt.get_tile(node).x) + ", " +
                                 std::to_string(lyt.get_tile(node).y) + ", " + std::to_string(lyt.get_tile(node).z) +
                                 "), is_po=" + std::to_string(lyt.is_po(node)) +
                                 ", is_wire=" + std::to_string(is_connection_wire(lyt, node))};
    }

    if (visit_state[node] == 1u)
    {
        throw std::runtime_error{"native hex GOLD fallback detected a structural dependency cycle"};
    }

    if (visit_state[node] == 2u)
    {
        return;
    }

    visit_state[node] = 1u;

    std::vector<mockturtle::signal<tec_nt>> children{};
    children.reserve(lyt.fanin_size(node));

    const auto fanin_collector = [&](const auto& fanin)
    {
        const auto traced_signal = trace_hex_structural_signal(lyt, fanin);
        const auto source_node   = lyt.get_node(traced_signal);

        ensure_mapped_hex_structural_node(lyt, ntk, old2new, visit_state, source_node);
        children.push_back(convert_hex_signal_to_technology_signal(lyt, ntk, old2new, traced_signal));
    };
    lyt.foreach_fanin(node, fanin_collector);

    auto& mapped_outputs = old2new[node];
    mapped_outputs.clear();

    if (lyt.is_multioutput(node))
    {
        mapped_outputs.reserve(node_output_pin_count(lyt, node));

        for (uint32_t pin = 0u; pin < node_output_pin_count(lyt, node); ++pin)
        {
            mapped_outputs.push_back(ntk.create_node(children, lyt.node_function_pin(node, pin)));
        }
    }
    else
    {
        mapped_outputs.push_back(ntk.create_node(children, lyt.node_function(node)));
    }

    visit_state[node] = 2u;
}

/**
 * @brief Reconstructs the structural technology network represented by a routed native hex layout.
 *
 * All routing wires and fanout buffers are discarded. Only the functional logic graph between PIs and POs is copied.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Source layout.
 * @return Routing-free technology network equivalent to `lyt`.
 */
template <typename Lyt>
[[nodiscard]] tec_nt extract_structural_hex_network(const Lyt& lyt)
{
    tec_nt                                                             ntk{};
    mockturtle::node_map<std::vector<mockturtle::signal<tec_nt>>, Lyt> old2new{lyt};
    std::vector<uint8_t>                                               visit_state(lyt.size(), 0u);
    const auto routing_objectives = extract_hex_routing_objectives(lyt);

    old2new[lyt.get_node(lyt.get_constant(false))] = {ntk.get_constant(false)};
    old2new[lyt.get_node(lyt.get_constant(true))]  = {ntk.get_constant(true)};

    lyt.foreach_pi([&ntk, &old2new](const auto& pi) { old2new[pi] = {ntk.create_pi()}; });

    visit_state[lyt.get_node(lyt.get_constant(false))] = 2u;
    visit_state[lyt.get_node(lyt.get_constant(true))]  = 2u;
    lyt.foreach_pi([&visit_state](const auto& pi) { visit_state[pi] = 2u; });

    lyt.foreach_gate(
        [&lyt, &ntk, &old2new, &visit_state](const auto& gate)
        {
            if (is_connection_wire(lyt, gate) || lyt.is_po(gate))
            {
                return;
            }

            ensure_mapped_hex_structural_node(lyt, ntk, old2new, visit_state, gate);
        });

    lyt.foreach_po(
        [&lyt, &ntk, &old2new, &visit_state, &routing_objectives](const auto& po)
        {
            auto       traced_signal = trace_hex_structural_signal(lyt, po);
            const auto traced_node   = lyt.get_node(traced_signal);

            if (lyt.is_po(traced_node))
            {
                const auto po_tile      = lyt.get_tile(traced_node);
                const auto po_objective = std::find_if(
                    routing_objectives.cbegin(), routing_objectives.cend(), [&po_tile](const auto& objective)
                    { return objective.target == po_tile && objective.source != po_tile; });

                if (po_objective == routing_objectives.cend())
                {
                    throw std::runtime_error{
                        "native hex GOLD fallback could not recover the structural driver for PO at tile (" +
                        std::to_string(po_tile.x) + ", " + std::to_string(po_tile.y) + ", " +
                        std::to_string(po_tile.z) + ")"};
                }

                traced_signal = lyt.make_signal(lyt.get_node(po_objective->source), po_objective->source_output);
            }

            ensure_mapped_hex_structural_node(lyt, ntk, old2new, visit_state, lyt.get_node(traced_signal));
            ntk.create_po(convert_hex_signal_to_technology_signal(lyt, ntk, old2new, traced_signal));
        });

    restore_names(lyt, ntk);

    return ntk;
}

/**
 * @brief Attempts a small-network GOLD-style fallback for native hex optimization.
 *
 * The fallback reconstructs the structural logic network from the routed layout and invokes the graph-oriented hex
 * placer on that reduced network. This is intentionally bounded to small structural networks because it is a global
 * restart rather than a local post-layout edit.
 *
 * @tparam Lyt Native hex gate-level layout type.
 * @param lyt Input layout.
 * @param ps Post-layout optimization parameters.
 * @return Improved candidate layout if the fallback succeeds and remains equivalent.
 */
template <typename Lyt>
[[nodiscard]] std::optional<Lyt> try_hex_gold_rebuild(const Lyt& lyt, const post_layout_optimization_params& ps)
{
    constexpr uint64_t structural_gate_limit = 24u;
    constexpr uint64_t fallback_timeout_ms   = 10000u;
    constexpr uint64_t minimum_height        = 16u;

    if (lyt.y() + 1u < minimum_height)
    {
        return std::nullopt;
    }

    if (count_hex_structural_gates(lyt) > structural_gate_limit)
    {
        return std::nullopt;
    }

    tec_nt ntk{};

    try
    {
        ntk = extract_structural_hex_network(lyt);
        ntk = mockturtle::cleanup_dangling(ntk, true, true);
    }
    catch (const std::runtime_error&)
    {
        return std::nullopt;
    }

    graph_oriented_layout_design_params gold_ps{};
    gold_ps.mode                      = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
    gold_ps.return_first              = true;
    gold_ps.enable_multithreading     = true;
    gold_ps.seed                      = 0u;
    gold_ps.tiles_to_skip_between_pis = 1u;
    gold_ps.timeout                   = std::min(ps.timeout, fallback_timeout_ms);
    gold_ps.planar                    = ps.planar_optimization;
    gold_ps.cost                      = graph_oriented_layout_design_params::cost_objective::AREA;

    auto candidate = graph_oriented_layout_design_hex<Lyt>(ntk, gold_ps);

    if (!candidate.has_value())
    {
        return std::nullopt;
    }

    compact_to_bounding_box(*candidate);

    if (equivalence_checking(lyt, *candidate) == eq_type::NO)
    {
        return std::nullopt;
    }

    return candidate;
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

        lyt.foreach_fanout(current_node,
                           [&](const auto& fanout_node)
                           {
                               const auto incoming_signal = find_incoming_signal_from(lyt, current_node, fanout_node);
                               const auto source_output =
                                   is_connection_wire(lyt, current_node) ?
                                       recent_source_output :
                                       (incoming_signal.has_value() ? incoming_signal->signal.output : uint8_t{0});
                               const auto target_input =
                                   incoming_signal.has_value() ? incoming_signal->index : uint32_t{0};
                               recursively_traverse_paths(current_gate_tile, source_output, target_input, fanout_node);
                           });
    };

    lyt.foreach_pi(
        [&](const auto& pi)
        {
            const auto pi_tile = lyt.get_tile(pi);

            lyt.foreach_fanout(pi,
                               [&](const auto& fanout_node)
                               {
                                   const auto incoming_signal = find_incoming_signal_from(lyt, pi, fanout_node);
                                   const auto source_output =
                                       incoming_signal.has_value() ? incoming_signal->signal.output : uint8_t{0};
                                   const auto target_input =
                                       incoming_signal.has_value() ? incoming_signal->index : uint32_t{0};
                                   recursively_traverse_paths(pi_tile, source_output, target_input, fanout_node);
                               });
        });

    sort_hex_routing_objectives(objectives);

    return objectives;
}

/**
 * @brief Removes all recreatable routing from a native hex layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout whose routing is to be deleted.
 */
template <typename Lyt>
void clear_hex_routing(Lyt& lyt) noexcept;

/**
 * @brief Re-routes a fixed native hex layout without moving gates.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to reroute.
 * @param routing_objectives Routing objectives to reconstruct.
 * @return `true` iff all routing objectives could be reconstructed.
 */
template <typename Lyt>
[[nodiscard]] bool reroute_hex_layout_wires(Lyt& lyt, const std::vector<hex_routing_objective<Lyt>>& routing_objectives,
                                            bool allow_crossings = false);

/**
 * @brief Removes vertically redundant wire-only rows from a native hex layout.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to compact.
 * @param allow_crossings Whether rerouting may use crossings.
 * @return `true` iff at least one row could be removed.
 */
template <typename Lyt>
[[nodiscard]] bool compact_hex_wire_rows(Lyt& lyt, bool allow_crossings);

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
 * @brief Returns the maximum row occupied by any non-PO structural node.
 *
 * Routing wires are ignored because they will be recreated after PO relocation.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Maximum occupied structural row.
 */
template <typename Lyt>
[[nodiscard]] uint64_t max_non_po_structural_row(const Lyt& lyt) noexcept
{
    uint64_t max_row{0u};

    lyt.foreach_node(
        [&lyt, &max_row](const auto& n)
        {
            if (lyt.is_constant(n) || lyt.is_po(n) || is_connection_wire(lyt, n))
            {
                return;
            }

            max_row = std::max(max_row, lyt.get_tile(n).y);
        });

    return max_row;
}

/**
 * @brief Returns a coarse top-left compactness score for structural gates.
 *
 * Lower scores indicate that non-PO structural gates are placed further toward the top-left corner.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Pair of summed row and column coordinates for structural gates.
 */
template <typename Lyt>
[[nodiscard]] std::pair<uint64_t, uint64_t> structural_gate_position_score(const Lyt& lyt) noexcept
{
    auto score = std::pair<uint64_t, uint64_t>{0u, 0u};

    lyt.foreach_node(
        [&lyt, &score](const auto& n)
        {
            if (lyt.is_constant(n) || lyt.is_pi(n) || lyt.is_po(n) || is_connection_wire(lyt, n))
            {
                return;
            }

            const auto gate_tile = lyt.get_tile(n);
            score.first += gate_tile.y;
            score.second += gate_tile.x;
        });

    return score;
}

/**
 * @brief Returns a lexicographic quality score for structural compactness and routing cost.
 *
 * Lower scores are better.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Tuple of structural row, structural score, wire count, and area.
 */
template <typename Lyt>
[[nodiscard]] std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t> hex_layout_quality(const Lyt& lyt) noexcept
{
    const auto gate_score = structural_gate_position_score(lyt);
    return {max_non_po_structural_row(lyt), gate_score.first, gate_score.second, internal_wire_count(lyt),
            layout_area(lyt)};
}

/**
 * @brief Returns whether a native hex candidate should replace a reference layout.
 *
 * The selector primarily targets compactness, but it allows a bounded increase in internal wires if the occupied
 * area shrinks enough to compensate. Ties are broken by @ref hex_layout_quality.
 *
 * @tparam Lyt Gate-level layout type.
 * @param candidate Candidate layout.
 * @param reference Reference layout.
 * @return `true` iff `candidate` is preferable to `reference`.
 */
template <typename Lyt>
[[nodiscard]] bool prefer_hex_layout_candidate(const Lyt& candidate, const Lyt& reference) noexcept
{
    constexpr uint64_t wire_area_tradeoff_weight = 16u;

    const auto candidate_wires = internal_wire_count(candidate);
    const auto reference_wires = internal_wire_count(reference);
    const auto candidate_area  = layout_area(candidate);
    const auto reference_area  = layout_area(reference);
    const auto candidate_cost  = candidate_area + wire_area_tradeoff_weight * candidate_wires;
    const auto reference_cost  = reference_area + wire_area_tradeoff_weight * reference_wires;

    if (candidate_cost != reference_cost)
    {
        return candidate_cost < reference_cost;
    }

    return hex_layout_quality(candidate) < hex_layout_quality(reference);
}

/**
 * @brief Enumerates border columns near a preferred x coordinate.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout that defines the column range.
 * @param preferred_x Preferred x coordinate.
 * @return Columns ordered by increasing distance from `preferred_x`.
 */
template <typename Lyt>
[[nodiscard]] std::vector<uint64_t> candidate_hex_po_columns(const Lyt& lyt, const uint64_t preferred_x)
{
    std::vector<uint64_t> columns{};
    columns.reserve(lyt.x() + 1u);

    if (preferred_x <= lyt.x())
    {
        columns.push_back(preferred_x);
    }

    for (uint64_t delta = 1u; delta <= lyt.x(); ++delta)
    {
        if (preferred_x >= delta)
        {
            columns.push_back(preferred_x - delta);
        }

        if (preferred_x + delta <= lyt.x())
        {
            columns.push_back(preferred_x + delta);
        }
    }

    return columns;
}

/**
 * @brief Collects routing metadata for all primary outputs in layout order.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param objectives Extracted routing objectives.
 * @return PO routing metadata ordered like `foreach_po`.
 */
template <typename Lyt>
[[nodiscard]] std::vector<hex_po_routing_data<Lyt>>
extract_hex_po_routing_data(const Lyt& lyt, const std::vector<hex_routing_objective<Lyt>>& objectives)
{
    std::vector<hex_po_routing_data<Lyt>> po_data{};
    po_data.reserve(lyt.num_pos());

    lyt.foreach_po(
        [&lyt, &objectives, &po_data](const auto& po, const auto)
        {
            const auto po_tile = lyt.get_tile(lyt.get_node(po));

            if (const auto objective_it =
                    std::find_if(objectives.cbegin(), objectives.cend(),
                                 [&po_tile](const auto& objective) { return objective.target == po_tile; });
                objective_it != objectives.cend())
            {
                po_data.push_back(
                    {static_cast<std::size_t>(std::distance(objectives.cbegin(), objective_it)), po_tile, po_tile.x});
            }
        });

    return po_data;
}

/**
 * @brief Attempts to move all POs onto a smaller common bottom border row.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to optimize.
 * @return `true` iff a strictly better PO border row was found.
 */
template <typename Lyt>
[[nodiscard]] bool optimize_hex_output_positions(Lyt& lyt, const bool allow_crossings)
{
    auto improved = false;
    auto progress = true;

    while (progress && lyt.y() > 0u)
    {
        progress = false;

        const auto baseline_area  = layout_area(lyt);
        const auto baseline_wires = internal_wire_count(lyt);
        const auto min_po_row     = max_non_po_structural_row(lyt) + 1u;

        if (lyt.y() <= min_po_row)
        {
            break;
        }

        for (uint64_t delta = 1u; delta <= lyt.y() - min_po_row; ++delta)
        {
            const auto po_row             = lyt.y() - delta;
            auto       candidate          = lyt.clone();
            auto       routing_objectives = extract_hex_routing_objectives(candidate);
            const auto po_data            = extract_hex_po_routing_data(candidate, routing_objectives);

            if (po_data.size() != candidate.num_pos())
            {
                break;
            }

            clear_hex_routing(candidate);

            auto placement_failed = false;

            for (const auto& po_item : po_data)
            {
                auto placed = false;

                for (const auto column : candidate_hex_po_columns(candidate, po_item.preferred_x))
                {
                    const auto po_tile = tile<Lyt>{column, po_row, 0u};

                    if (!candidate.is_empty_tile(po_tile))
                    {
                        continue;
                    }

                    candidate.move_node(candidate.get_node(po_item.current_target), po_tile, {});
                    routing_objectives[po_item.objective_index].target       = po_tile;
                    routing_objectives[po_item.objective_index].target_input = 0u;
                    placed                                                   = true;
                    break;
                }

                if (!placed)
                {
                    placement_failed = true;
                    break;
                }
            }

            if (placement_failed)
            {
                continue;
            }

            if (!reroute_hex_layout_wires(candidate, routing_objectives, allow_crossings))
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
            const auto improves_layout = (candidate_wires < baseline_wires) ||
                                         (candidate_wires == baseline_wires && candidate_area < baseline_area);

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
 * @brief Appends a tile to a vector only if it is not present already.
 *
 * @tparam Lyt Gate-level layout type.
 * @param tiles Tile list to update.
 * @param t Tile to append.
 */
template <typename Lyt>
void append_unique_hex_tile(std::vector<tile<Lyt>>& tiles, const tile<Lyt>& t)
{
    if (std::find(tiles.cbegin(), tiles.cend(), t) == tiles.cend())
    {
        tiles.push_back(t);
    }
}

/**
 * @brief Extracts the local routing neighborhood of a structural gate.
 *
 * Only single-owner routing chains are included in `to_clear`. Shared branch points remain in place and act as fixed
 * rerouting endpoints, mirroring the cartesian relocation strategy.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param gate_tile Tile of the structural gate to move.
 * @return Local fan-ins, fan-outs, and recreatable wires around the moved gate.
 */
template <typename Lyt>
[[nodiscard]] hex_local_relocation_data<Lyt> extract_hex_local_relocation_data(const Lyt&       lyt,
                                                                               const tile<Lyt>& gate_tile)
{
    auto data = hex_local_relocation_data<Lyt>{};

    const auto gate_node = lyt.get_node(gate_tile);
    uint32_t   gate_input{0u};

    lyt.foreach_fanin(gate_node,
                      [&lyt, &data, &gate_input](const auto& fin)
                      {
                          auto source_signal = fin;
                          auto source_node   = lyt.get_node(source_signal);

                          while (is_connection_wire(lyt, source_node) && lyt.fanout_size(source_node) == 1u &&
                                 !lyt.is_pi(source_node))
                          {
                              append_unique_hex_tile<Lyt>(data.to_clear, lyt.get_tile(source_node));

                              std::vector<mockturtle::signal<Lyt>> incoming_signals{};
                              lyt.foreach_fanin(source_node, [&incoming_signals](const auto& in)
                                                { incoming_signals.push_back(in); });

                              if (incoming_signals.empty())
                              {
                                  break;
                              }

                              source_signal = incoming_signals.front();
                              source_node   = lyt.get_node(source_signal);
                          }

                          data.fanins.push_back({source_signal, gate_input});
                          ++gate_input;
                      });

    lyt.foreach_fanout(gate_node,
                       [&lyt, &data, &gate_node](const auto& immediate_target)
                       {
                           const auto immediate_signal = find_incoming_signal_from(lyt, gate_node, immediate_target);

                           auto current_node = immediate_target;
                           auto source_output =
                               immediate_signal.has_value() ? immediate_signal->signal.output : uint8_t{0};
                           auto target_input = immediate_signal.has_value() ? immediate_signal->index : uint32_t{0};

                           while (is_connection_wire(lyt, current_node) && lyt.fanout_size(current_node) == 1u &&
                                  !lyt.is_po(current_node))
                           {
                               append_unique_hex_tile<Lyt>(data.to_clear, lyt.get_tile(current_node));

                               const auto outgoing_tiles = lyt.outgoing_data_flow(lyt.get_tile(current_node));

                               if (outgoing_tiles.empty())
                               {
                                   break;
                               }

                               const auto next_node   = lyt.get_node(outgoing_tiles.front());
                               const auto next_signal = find_incoming_signal_from(lyt, current_node, next_node);

                               current_node = next_node;
                               target_input = next_signal.has_value() ? next_signal->index : uint32_t{0};
                           }

                           data.fanouts.push_back({lyt.get_tile(current_node), source_output, target_input});
                       });

    return data;
}

/**
 * @brief Returns whether a tile may host a relocated structural gate after local routing cleanup.
 *
 * Both the ground tile and its crossing layer counterpart must be empty or part of the gate's recreatable local
 * routing.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param candidate_tile Ground-layer candidate tile.
 * @param reclaimable_tiles Local routing wires that will be deleted before rerouting.
 * @return `true` iff the candidate tile is available for relocation.
 */
template <typename Lyt>
[[nodiscard]] bool is_available_hex_gate_target(const Lyt& lyt, const tile<Lyt>& candidate_tile,
                                                const std::vector<tile<Lyt>>& reclaimable_tiles)
{
    const auto is_empty_or_reclaimable = [&lyt, &reclaimable_tiles](const auto& t)
    {
        return lyt.is_empty_tile(t) ||
               std::find(reclaimable_tiles.cbegin(), reclaimable_tiles.cend(), t) != reclaimable_tiles.cend();
    };

    if (!is_empty_or_reclaimable(candidate_tile))
    {
        return false;
    }

    if (candidate_tile.z < lyt.z())
    {
        return is_empty_or_reclaimable(lyt.above(candidate_tile));
    }

    return true;
}

/**
 * @brief Creates the wire chain for a routed path and returns the signal that reaches the path target.
 *
 * The path target itself is not connected here so that callers can preserve exact input ordering explicitly.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Path Path type.
 * @param lyt Layout.
 * @param source_signal Driving signal at the path source.
 * @param path Routed path from source to target.
 * @return Signal that arrives at the path target.
 */
template <typename Lyt, typename Path>
[[nodiscard]] mockturtle::signal<Lyt> route_hex_path_signal(Lyt& lyt, const mockturtle::signal<Lyt>& source_signal,
                                                            const Path& path)
{
    auto incoming_signal = source_signal;

    if (path.size() <= 2u)
    {
        return incoming_signal;
    }

    std::for_each(
        path.cbegin() + 1, path.cend() - 1, [&lyt, &incoming_signal](const auto& coord)
        { incoming_signal = lyt.create_buf(incoming_signal, lyt.is_empty_tile(coord) ? coord : lyt.above(coord)); });

    return incoming_signal;
}

/**
 * @brief Compares coordinates lexicographically.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lhs Left-hand coordinate.
 * @param rhs Right-hand coordinate.
 * @return `true` iff `lhs` precedes `rhs` lexicographically.
 */
template <typename Lyt>
[[nodiscard]] bool lexicographically_less(const coordinate<Lyt>& lhs, const coordinate<Lyt>& rhs) noexcept
{
    return std::tie(lhs.x, lhs.y, lhs.z) < std::tie(rhs.x, rhs.y, rhs.z);
}

/**
 * @brief Attempts to relocate one structural gate while rerouting only its local neighborhood.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Original layout.
 * @param old_tile Current gate tile.
 * @param new_tile Candidate gate tile.
 * @param local_data Local routing neighborhood around the moved gate.
 * @return Relocated layout if the local reroute succeeded.
 */
template <typename Lyt>
[[nodiscard]] std::optional<Lyt>
try_local_hex_gate_relocation(const Lyt& lyt, const tile<Lyt>& old_tile, const tile<Lyt>& new_tile,
                              const hex_local_relocation_data<Lyt>& local_data, const bool allow_crossings)
{
    auto       candidate = lyt.clone();
    const auto node      = candidate.get_node(old_tile);

    for (const auto& t : local_data.to_clear)
    {
        candidate.clear_tile(t);
    }

    auto grouped_fanouts = local_data.fanouts;
    std::sort(grouped_fanouts.begin(), grouped_fanouts.end(),
              [](const auto& lhs, const auto& rhs)
              {
                  if (lhs.target != rhs.target)
                  {
                      return lexicographically_less<Lyt>(lhs.target, rhs.target);
                  }

                  return lhs.target_input < rhs.target_input;
              });

    std::vector<tile<Lyt>>                            target_tiles{};
    std::vector<std::vector<mockturtle::signal<Lyt>>> target_children{};

    for (std::size_t i = 0u; i < grouped_fanouts.size();)
    {
        const auto& target_tile = grouped_fanouts[i].target;
        const auto  target_node = candidate.get_node(target_tile);

        std::vector<mockturtle::signal<Lyt>> children{};
        candidate.foreach_fanin(target_node, [&children](const auto& fin) { children.push_back(fin); });

        auto j = i;
        while (j < grouped_fanouts.size() && grouped_fanouts[j].target == target_tile)
        {
            ++j;
        }

        for (auto k = j; k-- > i;)
        {
            if (grouped_fanouts[k].target_input < children.size())
            {
                children.erase(children.cbegin() + static_cast<int64_t>(grouped_fanouts[k].target_input));
            }
        }

        candidate.move_node(target_node, target_tile, children);
        target_tiles.push_back(target_tile);
        target_children.push_back(children);

        i = j;
    }

    candidate.move_node(node, new_tile, {});

    a_star_params params{};
    params.crossings = allow_crossings;

    auto routed_fanins = local_data.fanins;
    std::sort(
        routed_fanins.begin(), routed_fanins.end(),
        [&new_tile](const auto& lhs, const auto& rhs)
        {
            const auto lhs_tile = static_cast<tile<Lyt>>(lhs.source_signal);
            const auto rhs_tile = static_cast<tile<Lyt>>(rhs.source_signal);
            const auto lhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs_tile.x) - static_cast<int64_t>(new_tile.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs_tile.y) - static_cast<int64_t>(new_tile.y)));
            const auto rhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs_tile.x) - static_cast<int64_t>(new_tile.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs_tile.y) - static_cast<int64_t>(new_tile.y)));
            return lhs_distance > rhs_distance;
        });

    std::vector<mockturtle::signal<Lyt>> moved_gate_children(local_data.fanins.size());

    for (const auto& fanin : routed_fanins)
    {
        const auto source_tile = static_cast<tile<Lyt>>(fanin.source_signal);
        const auto path        = a_star<layout_coordinate_path<Lyt>>(
            candidate, {source_tile, new_tile}, euclidean_distance_functor<Lyt>(), unit_cost_functor<Lyt>(), params);

        if (path.empty() && source_tile != new_tile)
        {
            return std::nullopt;
        }

        moved_gate_children[fanin.gate_input] =
            path.empty() ? fanin.source_signal : route_hex_path_signal(candidate, fanin.source_signal, path);
    }

    candidate.move_node(node, new_tile, moved_gate_children);

    auto routed_fanouts = local_data.fanouts;
    std::sort(
        routed_fanouts.begin(), routed_fanouts.end(),
        [&new_tile](const auto& lhs, const auto& rhs)
        {
            const auto lhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.target.x) - static_cast<int64_t>(new_tile.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(lhs.target.y) - static_cast<int64_t>(new_tile.y)));
            const auto rhs_distance =
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.target.x) - static_cast<int64_t>(new_tile.x))) +
                static_cast<uint64_t>(std::abs(static_cast<int64_t>(rhs.target.y) - static_cast<int64_t>(new_tile.y)));
            return lhs_distance > rhs_distance;
        });

    struct routed_fanout_signal
    {
        hex_local_fanout<Lyt>   fanout{};
        mockturtle::signal<Lyt> signal{};
    };

    std::vector<routed_fanout_signal> routed_fanout_signals{};
    routed_fanout_signals.reserve(routed_fanouts.size());

    for (const auto& fanout : routed_fanouts)
    {
        const auto path = a_star<layout_coordinate_path<Lyt>>(
            candidate, {new_tile, fanout.target}, euclidean_distance_functor<Lyt>(), unit_cost_functor<Lyt>(), params);

        if (path.empty() && new_tile != fanout.target)
        {
            return std::nullopt;
        }

        const auto source_signal = candidate.make_signal(node, fanout.source_output);
        routed_fanout_signals.push_back(
            {fanout, path.empty() ? source_signal : route_hex_path_signal(candidate, source_signal, path)});
    }

    std::sort(routed_fanout_signals.begin(), routed_fanout_signals.end(),
              [](const auto& lhs, const auto& rhs)
              {
                  if (lhs.fanout.target != rhs.fanout.target)
                  {
                      return lexicographically_less<Lyt>(lhs.fanout.target, rhs.fanout.target);
                  }

                  return lhs.fanout.target_input < rhs.fanout.target_input;
              });

    for (const auto& routed_fanout : routed_fanout_signals)
    {
        const auto target_it = std::find(target_tiles.cbegin(), target_tiles.cend(), routed_fanout.fanout.target);

        if (target_it == target_tiles.cend())
        {
            return std::nullopt;
        }

        const auto target_index = static_cast<std::size_t>(std::distance(target_tiles.cbegin(), target_it));
        auto&      children     = target_children[target_index];

        if (routed_fanout.fanout.target_input > children.size())
        {
            return std::nullopt;
        }

        children.insert(children.cbegin() + static_cast<int64_t>(routed_fanout.fanout.target_input),
                        routed_fanout.signal);
        candidate.move_node(candidate.get_node(routed_fanout.fanout.target), routed_fanout.fanout.target, children);
    }

    return candidate;
}

/**
 * @brief Attempts to relocate one structural gate and then reroute the entire layout from scratch.
 *
 * This is a fallback for cases where local neighborhood rerouting fails although the moved geometry may still be
 * globally routable after a complete rip-up and reroute.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Original layout.
 * @param old_tile Current gate tile.
 * @param new_tile Candidate gate tile.
 * @param local_data Local routing neighborhood around the moved gate.
 * @param allow_crossings Whether rerouting may use crossings.
 * @return Relocated layout if the global reroute succeeded.
 */
template <typename Lyt>
[[nodiscard]] std::optional<Lyt>
try_global_hex_gate_relocation(const Lyt& lyt, const tile<Lyt>& old_tile, const tile<Lyt>& new_tile,
                               const hex_local_relocation_data<Lyt>& local_data, const bool allow_crossings)
{
    auto       candidate = lyt.clone();
    const auto node      = candidate.get_node(old_tile);

    std::vector<mockturtle::signal<Lyt>> moved_gate_children(local_data.fanins.size());

    for (const auto& fanin : local_data.fanins)
    {
        if (fanin.gate_input >= moved_gate_children.size())
        {
            return std::nullopt;
        }

        moved_gate_children[fanin.gate_input] = fanin.source_signal;
    }

    candidate.move_node(node, new_tile, moved_gate_children);

    for (const auto& fanout : local_data.fanouts)
    {
        const auto                           target_node = candidate.get_node(fanout.target);
        std::vector<mockturtle::signal<Lyt>> children{};
        candidate.foreach_fanin(target_node, [&children](const auto& fin) { children.push_back(fin); });

        if (fanout.target_input >= children.size())
        {
            return std::nullopt;
        }

        children[fanout.target_input] = candidate.make_signal(node, fanout.source_output);
        candidate.move_node(target_node, fanout.target, children);
    }

    const auto objectives = extract_hex_routing_objectives(candidate);
    clear_hex_routing(candidate);

    if (!reroute_hex_layout_wires(candidate, objectives, allow_crossings))
    {
        return std::nullopt;
    }

    return candidate;
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

    std::sort(nodes_to_shift.begin(), nodes_to_shift.end(),
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
 * @brief Checks whether all structural nodes below a cut row can be shifted upward by one row without collisions.
 *
 * Routing wires are ignored because they are removed before the shift is applied and reconstructed afterward.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param row Last row that stays fixed.
 * @return `true` iff shifting every structural node with `y > row` to `y - 1` is tile-wise legal.
 */
template <typename Lyt>
[[nodiscard]] bool can_shift_hex_structural_suffix_up(const Lyt& lyt, const uint64_t row) noexcept
{
    const auto tile_less = [](const auto& lhs, const auto& rhs)
    {
        if (lhs.y != rhs.y)
        {
            return lhs.y < rhs.y;
        }

        if (lhs.x != rhs.x)
        {
            return lhs.x < rhs.x;
        }

        return lhs.z < rhs.z;
    };

    std::vector<tile<Lyt>> fixed_tiles{};
    fixed_tiles.reserve(lyt.num_gates() + lyt.num_pis() + lyt.num_pos());
    auto has_shifted_node = false;

    lyt.foreach_node(
        [&lyt, row, &fixed_tiles, &has_shifted_node](const auto& n)
        {
            if (lyt.is_constant(n) || is_connection_wire(lyt, n))
            {
                return;
            }

            const auto node_tile = lyt.get_tile(n);

            if (node_tile.is_dead())
            {
                return;
            }

            if (node_tile.y <= row)
            {
                fixed_tiles.push_back(node_tile);
            }
            else
            {
                has_shifted_node = true;
            }
        });

    if (!has_shifted_node)
    {
        return false;
    }

    std::sort(fixed_tiles.begin(), fixed_tiles.end(), tile_less);

    auto shift_is_legal = true;

    lyt.foreach_node(
        [&lyt, row, &fixed_tiles, &shift_is_legal, &tile_less](const auto& n)
        {
            if (!shift_is_legal || lyt.is_constant(n) || is_connection_wire(lyt, n))
            {
                return;
            }

            const auto node_tile = lyt.get_tile(n);

            if (node_tile.is_dead() || node_tile.y <= row)
            {
                return;
            }

            auto target_tile = node_tile;
            --target_tile.y;

            shift_is_legal = !std::binary_search(fixed_tiles.cbegin(), fixed_tiles.cend(), target_tile, tile_less);
        });

    return shift_is_legal;
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
[[nodiscard]] bool reroute_hex_layout_wires(Lyt& lyt, const std::vector<hex_routing_objective<Lyt>>& routing_objectives,
                                            const bool allow_crossings)
{
    if (routing_objectives.empty())
    {
        return true;
    }

    const auto original_layout = lyt.clone();

    clear_hex_routing(lyt);

    a_star_params params{};
    params.crossings = allow_crossings;

    for (const auto& objective : routing_objectives)
    {
        if (objective.source == objective.target)
        {
            continue;
        }

        const auto path =
            a_star<layout_coordinate_path<Lyt>>(lyt, {objective.source, objective.target},
                                                euclidean_distance_functor<Lyt>(), unit_cost_functor<Lyt>(), params);

        if (path.empty())
        {
            lyt = original_layout;
            return false;
        }

        auto incoming_signal = lyt.make_signal(lyt.get_node(objective.source), objective.source_output);

        std::for_each(path.cbegin() + 1, path.cend() - 1,
                      [&lyt, &incoming_signal](const auto& coord)
                      {
                          incoming_signal =
                              lyt.create_buf(incoming_signal, lyt.is_empty_tile(coord) ? coord : lyt.above(coord));
                      });

        const auto target_node = lyt.get_node(path.target());
        auto       target_tile = lyt.get_tile(target_node);

        std::vector<mockturtle::signal<Lyt>> target_children{};
        lyt.foreach_fanin(target_node, [&target_children](const auto& fin) { target_children.push_back(fin); });

        if (objective.target_input > target_children.size())
        {
            lyt = original_layout;
            return false;
        }

        target_children.insert(target_children.cbegin() + static_cast<int64_t>(objective.target_input),
                               incoming_signal);
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
[[nodiscard]] bool reroute_hex_layout_wires(Lyt& lyt, const bool allow_crossings)
{
    return reroute_hex_layout_wires(lyt, extract_hex_routing_objectives(lyt), allow_crossings);
}

/**
 * @brief Tries a full native-hex reroute and keeps it only if it improves the given candidate.
 *
 * The comparison is wire-first and area-second, mirroring the final native-hex reroute selection policy.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Candidate layout to refine.
 * @param allow_crossings Whether rerouting may use crossings.
 * @return Refined layout if global reroute improved it, otherwise the original candidate.
 */
template <typename Lyt>
[[nodiscard]] Lyt refine_hex_layout_with_global_reroute(const Lyt& lyt, const bool allow_crossings)
{
    auto best_layout     = lyt.clone();
    auto rerouted_layout = best_layout.clone();

    if (!reroute_hex_layout_wires(rerouted_layout, allow_crossings))
    {
        return best_layout;
    }

    [[maybe_unused]] const auto rerouted_po_optimization =
        optimize_hex_output_positions(rerouted_layout, allow_crossings);
    [[maybe_unused]] const auto rerouted_wire_row_compaction = compact_hex_wire_rows(rerouted_layout, allow_crossings);
    [[maybe_unused]] const auto rerouted_po_optimization_after_row_compaction =
        optimize_hex_output_positions(rerouted_layout, allow_crossings);
    compact_to_bounding_box(rerouted_layout);

    const auto equivalent_layouts = equivalence_checking(best_layout, rerouted_layout) != eq_type::NO;

    if (!equivalent_layouts)
    {
        return best_layout;
    }

    const auto best_area           = layout_area(best_layout);
    const auto rerouted_area       = layout_area(rerouted_layout);
    const auto best_wire_count     = internal_wire_count(best_layout);
    const auto rerouted_wire_count = internal_wire_count(rerouted_layout);
    const auto improves_layout     = (rerouted_wire_count < best_wire_count) ||
                                 (rerouted_wire_count == best_wire_count && rerouted_area <= best_area);

    return improves_layout ? std::move(rerouted_layout) : std::move(best_layout);
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
[[nodiscard]] bool compact_hex_wire_rows(Lyt& lyt, const bool allow_crossings)
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

            auto       candidate  = lyt.clone();
            const auto objectives = shift_objectives_after_row_removal(extract_hex_routing_objectives(candidate), row);
            const auto current_area  = layout_area(lyt);
            const auto current_wires = internal_wire_count(lyt);

            clear_hex_routing(candidate);
            shift_structural_nodes_up_after_row_removal(candidate, row);

            if (!reroute_hex_layout_wires(candidate, objectives, allow_crossings))
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
            const auto improves_layout = (candidate_wires < current_wires) ||
                                         (candidate_wires == current_wires && candidate_area < current_area);

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

/**
 * @brief Compacts native hex layouts by shifting structural suffixes upward row by row.
 *
 * For each cut row, all structural nodes below it are moved one row upward together if this does not create
 * tile conflicts. Routing is then rebuilt globally, followed by PO and wire-row cleanup.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to optimize.
 * @param allow_crossings Whether rerouting may use crossings.
 * @return `true` iff at least one structural suffix shift was accepted.
 */
template <typename Lyt>
[[nodiscard]] bool compact_hex_structural_suffixes(Lyt& lyt, const bool allow_crossings)
{
    constexpr uint64_t exploratory_wire_slack = 8u;

    auto improved = false;
    auto progress = true;

    while (progress && lyt.y() > 0u)
    {
        progress = false;

        const auto baseline_area       = layout_area(lyt);
        const auto baseline_wires      = internal_wire_count(lyt);
        const auto baseline_non_po_row = max_non_po_structural_row(lyt);
        const auto baseline_gate_score = structural_gate_position_score(lyt);

        for (uint64_t row = 0u; row < lyt.y(); ++row)
        {
            if (!can_shift_hex_structural_suffix_up(lyt, row))
            {
                continue;
            }

            auto candidate = lyt.clone();

            clear_hex_routing(candidate);
            shift_structural_nodes_up_after_row_removal(candidate, row);

            if (!reroute_hex_layout_wires(candidate, allow_crossings))
            {
                continue;
            }

            [[maybe_unused]] const auto optimized_po_positions =
                optimize_hex_output_positions(candidate, allow_crossings);
            [[maybe_unused]] const auto removed_wire_rows = compact_hex_wire_rows(candidate, allow_crossings);
            [[maybe_unused]] const auto optimized_po_positions_after_row_compaction =
                optimize_hex_output_positions(candidate, allow_crossings);
            compact_to_bounding_box(candidate);
            candidate = refine_hex_layout_with_global_reroute(candidate, allow_crossings);

            if (equivalence_checking(lyt, candidate) == eq_type::NO)
            {
                continue;
            }

            const auto candidate_area           = layout_area(candidate);
            const auto candidate_wires          = internal_wire_count(candidate);
            const auto candidate_non_po_row     = max_non_po_structural_row(candidate);
            const auto candidate_gate_score     = structural_gate_position_score(candidate);
            const auto directly_improves_layout = candidate_area < baseline_area ||
                                                  candidate_non_po_row < baseline_non_po_row ||
                                                  candidate_gate_score < baseline_gate_score;
            const auto preserves_cost_bounds = candidate_area <= baseline_area && candidate_wires <= baseline_wires;
            const auto preserves_exploratory_bounds =
                candidate_area <= baseline_area && candidate_wires <= baseline_wires + exploratory_wire_slack &&
                (candidate_non_po_row < baseline_non_po_row || candidate_gate_score < baseline_gate_score);
            const auto improves_layout =
                directly_improves_layout && (preserves_cost_bounds || preserves_exploratory_bounds);

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

/**
 * @brief Collects all movable structural gate tiles in bottom-up order.
 *
 * PIs, POs, constants, and recreatable routing wires are excluded.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout.
 * @return Structural gate tiles ordered from bottom-right to top-left.
 */
template <typename Lyt>
[[nodiscard]] std::vector<tile<Lyt>> collect_movable_hex_gate_tiles(const Lyt& lyt)
{
    std::vector<tile<Lyt>> gate_tiles{};

    lyt.foreach_node(
        [&lyt, &gate_tiles](const auto& n)
        {
            if (lyt.is_constant(n) || lyt.is_pi(n) || lyt.is_po(n) || is_connection_wire(lyt, n))
            {
                return;
            }

            gate_tiles.push_back(lyt.get_tile(n));
        });

    std::sort(gate_tiles.begin(), gate_tiles.end(),
              [](const auto& lhs, const auto& rhs)
              {
                  if (lhs.y != rhs.y)
                  {
                      return lhs.y > rhs.y;
                  }

                  return lhs.x > rhs.x;
              });

    return gate_tiles;
}

/**
 * @brief Enumerates candidate relocation tiles for one structural gate.
 *
 * Candidates are ordered to prefer upward movement first and then leftward movement on the same row.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout that defines the current free space.
 * @param old_tile Current tile of the relocated gate.
 * @param max_candidates Maximum number of candidate tiles to enumerate.
 * @return Candidate tiles in priority order.
 */
template <typename Lyt>
[[nodiscard]] std::vector<tile<Lyt>> candidate_hex_gate_tiles(const Lyt& lyt, const tile<Lyt>& old_tile,
                                                              const std::vector<tile<Lyt>>& reclaimable_tiles,
                                                              const uint64_t                max_candidates)
{
    std::vector<tile<Lyt>> candidates{};
    candidates.reserve(std::min<uint64_t>((lyt.x() + 1u) * (old_tile.y + 1u), max_candidates));

    for (uint64_t y = 0u; y <= old_tile.y; ++y)
    {
        for (const auto x : candidate_hex_po_columns(lyt, old_tile.x))
        {
            if ((y == old_tile.y && x >= old_tile.x) || (x == old_tile.x && y == old_tile.y))
            {
                continue;
            }

            const auto candidate_tile = tile<Lyt>{x, y, 0u};

            if (!is_available_hex_gate_target(lyt, candidate_tile, reclaimable_tiles))
            {
                continue;
            }

            candidates.push_back(candidate_tile);

            if (candidates.size() >= max_candidates)
            {
                return candidates;
            }
        }
    }

    return candidates;
}

/**
 * @brief Attempts to relocate structural gates toward the top-left corner.
 *
 * Each accepted move is followed by routing reconstruction and the existing PO/row compaction passes.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to optimize.
 * @param ps Post-layout optimization parameters.
 * @return `true` iff at least one structural gate was relocated successfully.
 */
template <typename Lyt>
[[nodiscard]] bool optimize_hex_gate_positions(Lyt& lyt, const post_layout_optimization_params& ps)
{
    constexpr uint64_t exploratory_wire_slack = 8u;

    const auto relocation_budget = ps.max_gate_relocations.value_or((lyt.x() + 1u) * (lyt.y() + 1u));
    const auto allow_crossings   = !ps.planar_optimization;

    if (relocation_budget == 0u)
    {
        return false;
    }

    auto     improved            = false;
    auto     progress            = true;
    uint64_t relocation_attempts = 0u;

    while (progress && relocation_attempts < relocation_budget)
    {
        progress = false;

        const auto baseline_area       = layout_area(lyt);
        const auto baseline_wires      = internal_wire_count(lyt);
        const auto baseline_x          = lyt.x();
        const auto baseline_y          = lyt.y();
        const auto baseline_non_po_row = max_non_po_structural_row(lyt);
        const auto baseline_gate_score = structural_gate_position_score(lyt);

        for (const auto& old_tile : collect_movable_hex_gate_tiles(lyt))
        {
            if (relocation_attempts >= relocation_budget)
            {
                break;
            }

            const auto local_data       = extract_hex_local_relocation_data(lyt, old_tile);
            const auto remaining_budget = relocation_budget - relocation_attempts;

            for (const auto& new_tile : candidate_hex_gate_tiles(lyt, old_tile, local_data.to_clear, remaining_budget))
            {
                ++relocation_attempts;

                auto relocated_candidate =
                    try_local_hex_gate_relocation(lyt, old_tile, new_tile, local_data, allow_crossings);

                if (!relocated_candidate.has_value())
                {
                    relocated_candidate =
                        try_global_hex_gate_relocation(lyt, old_tile, new_tile, local_data, allow_crossings);
                }

                if (!relocated_candidate.has_value())
                {
                    continue;
                }

                auto candidate = std::move(*relocated_candidate);

                [[maybe_unused]] const auto optimized_po_positions =
                    optimize_hex_output_positions(candidate, allow_crossings);
                [[maybe_unused]] const auto removed_wire_rows = compact_hex_wire_rows(candidate, allow_crossings);
                [[maybe_unused]] const auto optimized_po_positions_after_row_compaction =
                    optimize_hex_output_positions(candidate, allow_crossings);
                compact_to_bounding_box(candidate);
                candidate = refine_hex_layout_with_global_reroute(candidate, allow_crossings);

                if (equivalence_checking(lyt, candidate) == eq_type::NO)
                {
                    continue;
                }

                const auto candidate_area        = layout_area(candidate);
                const auto candidate_wires       = internal_wire_count(candidate);
                const auto candidate_non_po_row  = max_non_po_structural_row(candidate);
                const auto candidate_gate_score  = structural_gate_position_score(candidate);
                const auto preserves_cost_bounds = candidate_area <= baseline_area && candidate_wires <= baseline_wires;
                const auto directly_improves_layout =
                    candidate_area < baseline_area || candidate_wires < baseline_wires || candidate.y() < baseline_y ||
                    candidate.x() < baseline_x || candidate_non_po_row < baseline_non_po_row;
                const auto improves_gate_compactness = candidate_gate_score < baseline_gate_score;
                const auto preserves_exploratory_bounds =
                    candidate_area <= baseline_area && candidate_wires <= baseline_wires + exploratory_wire_slack &&
                    (candidate_non_po_row < baseline_non_po_row || improves_gate_compactness);
                const auto improves_layout =
                    (preserves_cost_bounds && (directly_improves_layout || improves_gate_compactness)) ||
                    (preserves_exploratory_bounds &&
                     (candidate_non_po_row < baseline_non_po_row || improves_gate_compactness));

                if (!improves_layout)
                {
                    continue;
                }

                lyt      = std::move(candidate);
                improved = true;
                progress = true;
                break;
            }

            if (progress)
            {
                break;
            }
        }
    }

    return improved;
}

/**
 * @brief Collects a small beam of promising first-step relocation candidates for native hex optimization.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to explore from.
 * @param ps Post-layout optimization parameters.
 * @param beam_width Maximum number of candidates to keep.
 * @param candidate_budget Maximum number of relocation candidates to evaluate.
 * @return Beam of promising candidate layouts ordered by quality.
 */
template <typename Lyt>
[[nodiscard]] std::vector<Lyt>
collect_promising_hex_relocation_candidates(const Lyt& lyt, const post_layout_optimization_params& ps,
                                            const uint64_t beam_width, const uint64_t candidate_budget)
{
    constexpr uint64_t exploratory_wire_slack = 8u;

    if (beam_width == 0u || candidate_budget == 0u)
    {
        return {};
    }

    const auto allow_crossings  = !ps.planar_optimization;
    const auto baseline_area    = layout_area(lyt);
    const auto baseline_wires   = internal_wire_count(lyt);
    const auto baseline_quality = hex_layout_quality(lyt);

    std::vector<std::pair<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>, Lyt>> beam{};
    beam.reserve(beam_width);

    uint64_t evaluated_candidates = 0u;

    for (const auto& old_tile : collect_movable_hex_gate_tiles(lyt))
    {
        if (evaluated_candidates >= candidate_budget)
        {
            break;
        }

        const auto local_data       = extract_hex_local_relocation_data(lyt, old_tile);
        const auto remaining_budget = candidate_budget - evaluated_candidates;

        for (const auto& new_tile : candidate_hex_gate_tiles(lyt, old_tile, local_data.to_clear, remaining_budget))
        {
            ++evaluated_candidates;

            auto relocated_candidate =
                try_local_hex_gate_relocation(lyt, old_tile, new_tile, local_data, allow_crossings);

            if (!relocated_candidate.has_value())
            {
                relocated_candidate =
                    try_global_hex_gate_relocation(lyt, old_tile, new_tile, local_data, allow_crossings);
            }

            if (!relocated_candidate.has_value())
            {
                continue;
            }

            auto candidate = std::move(*relocated_candidate);

            [[maybe_unused]] const auto optimized_po_positions =
                optimize_hex_output_positions(candidate, allow_crossings);
            [[maybe_unused]] const auto removed_wire_rows = compact_hex_wire_rows(candidate, allow_crossings);
            [[maybe_unused]] const auto optimized_po_positions_after_row_compaction =
                optimize_hex_output_positions(candidate, allow_crossings);
            compact_to_bounding_box(candidate);
            candidate = refine_hex_layout_with_global_reroute(candidate, allow_crossings);

            if (equivalence_checking(lyt, candidate) == eq_type::NO)
            {
                continue;
            }

            const auto candidate_area    = layout_area(candidate);
            const auto candidate_wires   = internal_wire_count(candidate);
            const auto candidate_quality = hex_layout_quality(candidate);
            const auto within_bounds =
                candidate_area <= baseline_area && candidate_wires <= baseline_wires + exploratory_wire_slack;
            const auto promising = candidate_quality < baseline_quality || candidate.y() < lyt.y();

            if (!within_bounds || !promising)
            {
                continue;
            }

            beam.emplace_back(candidate_quality, std::move(candidate));

            std::sort(beam.begin(), beam.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

            if (beam.size() > beam_width)
            {
                beam.resize(beam_width);
            }
        }
    }

    std::vector<Lyt> candidates{};
    candidates.reserve(beam.size());

    for (auto& [quality, candidate] : beam)
    {
        static_cast<void>(quality);
        candidates.push_back(std::move(candidate));
    }

    return candidates;
}

/**
 * @brief Explores short relocation sequences by seeding the greedy optimizer with promising first moves.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @param lyt Layout to optimize.
 * @param ps Post-layout optimization parameters.
 * @return `true` iff a multi-step candidate improved the layout.
 */
template <typename Lyt>
[[nodiscard]] bool explore_two_step_hex_gate_relocations(Lyt& lyt, const post_layout_optimization_params& ps)
{
    constexpr uint64_t beam_width       = 4u;
    constexpr uint64_t candidate_budget = 64u;

    const auto allow_crossings  = !ps.planar_optimization;
    const auto original_area    = layout_area(lyt);
    const auto original_wires   = internal_wire_count(lyt);
    const auto original_quality = hex_layout_quality(lyt);

    auto best_candidate = lyt.clone();
    auto improved       = false;

    for (auto seed : collect_promising_hex_relocation_candidates(lyt, ps, beam_width, candidate_budget))
    {
        [[maybe_unused]] const auto relocated_more_gates   = optimize_hex_gate_positions(seed, ps);
        [[maybe_unused]] const auto compacted_suffixes     = compact_hex_structural_suffixes(seed, allow_crossings);
        [[maybe_unused]] const auto optimized_po_positions = optimize_hex_output_positions(seed, allow_crossings);
        [[maybe_unused]] const auto removed_wire_rows      = compact_hex_wire_rows(seed, allow_crossings);
        [[maybe_unused]] const auto optimized_po_positions_after_row_compaction =
            optimize_hex_output_positions(seed, allow_crossings);
        compact_to_bounding_box(seed);
        seed = refine_hex_layout_with_global_reroute(seed, allow_crossings);

        if (equivalence_checking(lyt, seed) == eq_type::NO)
        {
            continue;
        }

        const auto seed_area    = layout_area(seed);
        const auto seed_wires   = internal_wire_count(seed);
        const auto seed_quality = hex_layout_quality(seed);
        const auto best_quality = hex_layout_quality(best_candidate);
        const auto beats_original =
            (seed_wires < original_wires) || (seed_wires == original_wires && seed_area <= original_area);
        const auto beats_best = seed_quality < best_quality;

        if (beats_original && (seed_quality < original_quality) && (!improved || beats_best))
        {
            best_candidate = std::move(seed);
            improved       = true;
        }
    }

    if (improved)
    {
        lyt = std::move(best_candidate);
    }

    return improved;
}

}  // namespace detail

/**
 * @brief Post-layout optimization for native row-clocked pointy-top hexagonal gate-level layouts.
 *
 * Native hexagonal layouts are optimized in three conservative steps:
 * 1. compact the occupied bounding box,
 * 2. move primary outputs upward to the lowest feasible bottom border,
 * 3. remove vertically redundant rows that contain routing wires only,
 * 4. shift conflict-free structural suffixes upward when rerouting stays legal and beneficial,
 * 5. relocate structural gates toward the top-left corner if rerouting stays legal and beneficial,
 * 6. re-route existing connections with gates fixed in place to reduce excess wire detours.
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

        auto&      mutable_layout  = const_cast<Lyt&>(lyt);
        const auto allow_crossings = !ps.planar_optimization;

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

        auto       best_layout     = lyt.clone();
        const auto original_layout = lyt.clone();
        detail::compact_to_bounding_box(best_layout);
        [[maybe_unused]] const auto optimized_po_positions =
            detail::optimize_hex_output_positions(best_layout, allow_crossings);
        [[maybe_unused]] const auto removed_hex_rows = detail::compact_hex_wire_rows(best_layout, allow_crossings);
        [[maybe_unused]] const auto optimized_po_positions_after_row_compaction =
            detail::optimize_hex_output_positions(best_layout, allow_crossings);
        [[maybe_unused]] const auto compacted_hex_suffixes =
            detail::compact_hex_structural_suffixes(best_layout, allow_crossings);
        [[maybe_unused]] const auto relocated_hex_gates = detail::optimize_hex_gate_positions(best_layout, ps);
        [[maybe_unused]] const auto explored_two_step_hex_relocations =
            detail::explore_two_step_hex_gate_relocations(best_layout, ps);
        detail::compact_to_bounding_box(best_layout);

        best_layout = detail::refine_hex_layout_with_global_reroute(best_layout, allow_crossings);

        if (const auto gold_candidate = detail::try_hex_gold_rebuild(original_layout, ps); gold_candidate.has_value())
        {
            auto                        polished_gold = *gold_candidate;
            [[maybe_unused]] const auto optimized_gold_po_positions =
                detail::optimize_hex_output_positions(polished_gold, allow_crossings);
            [[maybe_unused]] const auto removed_gold_wire_rows =
                detail::compact_hex_wire_rows(polished_gold, allow_crossings);
            [[maybe_unused]] const auto optimized_gold_po_positions_after_row_compaction =
                detail::optimize_hex_output_positions(polished_gold, allow_crossings);
            [[maybe_unused]] const auto compacted_gold_suffixes =
                detail::compact_hex_structural_suffixes(polished_gold, allow_crossings);
            [[maybe_unused]] const auto relocated_gold_gates = detail::optimize_hex_gate_positions(polished_gold, ps);
            [[maybe_unused]] const auto explored_gold_two_step_relocations =
                detail::explore_two_step_hex_gate_relocations(polished_gold, ps);
            detail::compact_to_bounding_box(polished_gold);
            polished_gold = detail::refine_hex_layout_with_global_reroute(polished_gold, allow_crossings);

            if (detail::prefer_hex_layout_candidate(polished_gold, best_layout))
            {
                best_layout = std::move(polished_gold);
            }
        }

        const auto keeps_equivalence = equivalence_checking(original_layout, best_layout) != eq_type::NO;
        const auto improves_original =
            detail::prefer_hex_layout_candidate(best_layout, original_layout) ||
            (detail::layout_area(best_layout) == detail::layout_area(original_layout) &&
             detail::internal_wire_count(best_layout) == detail::internal_wire_count(original_layout));

        mutable_layout = (keeps_equivalence && improves_original) ? best_layout : original_layout;

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
