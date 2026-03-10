//
// Created by marcel on 10.02.26.
//

#ifndef FICTION_UNSCRAMBLE_PINS_HPP
#define FICTION_UNSCRAMBLE_PINS_HPP

#include "fiction/algorithms/path_finding/a_star.hpp"
#include "fiction/layouts/obstruction_layout.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/debug/network_writer.hpp"
#include "fiction/utils/routing_utils.hpp"

#include <fmt/format.h>
#include <mockturtle/traits.hpp>
#include <mockturtle/utils/node_map.hpp>
#include <mockturtle/utils/stopwatch.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiction
{

/**
 * Parameters for the pin unscrambling algorithm.
 */
struct unscramble_pins_params
{};
/**
 * Statistics for the pin unscrambling algorithm.
 */
struct unscramble_pins_stats
{
    /**
     * Total runtime of the algorithm.
     */
    mockturtle::stopwatch<>::duration time_total{0};

    /**
     * Reports collected runtime statistics.
     *
     * @param out Output stream.
     */
    void report(std::ostream& out = std::cout) const
    {
        out << fmt::format("[i] total time = {:.2f} secs\n", mockturtle::to_seconds(time_total));
    }
};

namespace detail
{

/**
 * Returns the layout coordinate represented by a pin handle.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @param pin Pin handle represented as a node.
 * @return Coordinate of the referenced pin.
 */
template <typename Lyt>
[[nodiscard]] tile<Lyt> pin_coordinate(const Lyt& lyt, const mockturtle::node<Lyt>& pin)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    return lyt.get_tile(pin);
}
/**
 * Returns the layout coordinate represented by a pin handle.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @param pin Pin handle represented as a signal.
 * @return Coordinate of the referenced pin.
 */
template <typename Lyt>
[[nodiscard]] tile<Lyt> pin_coordinate(const Lyt& lyt, const mockturtle::signal<Lyt>& pin)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    static_cast<void>(lyt);

    return static_cast<tile<Lyt>>(pin);
}
/**
 * Extracts the layout coordinates for a given list of pin handles.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Pin Pin handle type.
 * @param lyt The layout.
 * @param pins Vector of pin handles.
 * @return Vector of coordinates corresponding to the pins.
 */
template <typename Lyt, typename Pin>
std::vector<tile<Lyt>> determine_pin_coordinates(const Lyt& lyt, const std::vector<Pin>& pins)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    std::vector<tile<Lyt>> coords{};
    coords.reserve(pins.size());
    for (const auto& pin : pins)
    {
        coords.push_back(pin_coordinate(lyt, pin));
    }

    return coords;
}
/**
 * Extracts the layout coordinates for a given list of pin handles and sorts them from left to right.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Pin Pin handle type.
 * @param lyt The layout.
 * @param pins Vector of pin handles.
 * @return Vector of coordinates sorted by physical slot order.
 */
template <typename Lyt, typename Pin>
std::vector<tile<Lyt>> determine_pin_slot_coordinates(const Lyt& lyt, const std::vector<Pin>& pins)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    auto coords = determine_pin_coordinates(lyt, pins);

    std::sort(coords.begin(), coords.end(),
              [](const auto& lhs, const auto& rhs)
              {
                  if (lhs.x != rhs.x)
                  {
                      return lhs.x < rhs.x;
                  }

                  if (lhs.y != rhs.y)
                  {
                      return lhs.y < rhs.y;
                  }

                  return lhs.z < rhs.z;
              });

    return coords;
}
/**
 * Extracts interface slot coordinates for a given list of pin handles and redistributes them across the available
 * layout width. This intentionally introduces horizontal gaps for the new external pin interface to avoid routing
 * deadlocks caused by densely packed boundary pins.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Pin Pin handle type.
 * @param lyt The layout.
 * @param pins Vector of pin handles.
 * @return Vector of coordinates sorted by physical slot order across the full interface width.
 */
template <typename Lyt, typename Pin>
std::vector<tile<Lyt>> determine_distributed_pin_slot_coordinates(const Lyt& lyt, const std::vector<Pin>& pins)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    auto coords = determine_pin_slot_coordinates(lyt, pins);

    if (coords.size() <= 1u)
    {
        return coords;
    }

    const auto current_span                  = static_cast<uint32_t>(coords.back().x - coords.front().x + 1u);
    const auto required_span_for_gap_per_pin = static_cast<uint32_t>(coords.size() * 2u - 1u);

    if (current_span >= required_span_for_gap_per_pin)
    {
        return coords;
    }

    const auto max_x = static_cast<uint32_t>(lyt.x());
    if (max_x + 1u < coords.size())
    {
        return coords;
    }

    std::vector<tile<Lyt>> distributed{};
    distributed.reserve(coords.size());

    const auto y = coords.front().y;
    const auto z = coords.front().z;

    uint32_t last_x = 0u;

    for (uint32_t i = 0u; i < coords.size(); ++i)
    {
        uint32_t x = 0u;

        if (i == 0u)
        {
            x = 0u;
        }
        else if (i + 1u == coords.size())
        {
            x = max_x;
        }
        else
        {
            const auto raw_x = static_cast<uint32_t>((static_cast<uint64_t>(i) * static_cast<uint64_t>(max_x)) /
                                                     static_cast<uint64_t>(coords.size() - 1u));
            const auto max_remaining_x = max_x - static_cast<uint32_t>((coords.size() - 1u) - i);

            x = std::max(raw_x, last_x + 1u);
            x = std::min(x, max_remaining_x);
        }

        distributed.emplace_back(tile<Lyt>{x, y, z});
        last_x = x;
    }

    return distributed;
}
/**
 * Calculates the horizontal permutation distances for each desired slot.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Pin Pin handle type.
 * @param lyt The layout.
 * @param current_permutation The current order of pins.
 * @param desired_permutation The desired order of pins.
 * @return Vector of horizontal distances per desired slot.
 */
template <typename Lyt, typename Pin>
std::vector<uint32_t> calculate_permutation_distances(const Lyt& lyt, const std::vector<Pin>& current_permutation,
                                                      const std::vector<Pin>& desired_permutation)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    std::vector<uint32_t> distances(desired_permutation.size(), 0u);

    if (desired_permutation.empty() || current_permutation.empty())
    {
        return distances;
    }

    const auto current_coords = determine_pin_coordinates(lyt, current_permutation);
    const auto target_slots   = determine_distributed_pin_slot_coordinates(lyt, current_permutation);

    for (size_t i = 0; i < desired_permutation.size(); ++i)
    {
        if (i >= current_coords.size())
        {
            break;
        }

        const auto target_node = desired_permutation[i];

        // Find where this node is currently located
        const auto it = std::find(current_permutation.cbegin(), current_permutation.cend(), target_node);
        if (it == current_permutation.cend())
        {
            continue;
        }

        // Current location of the node
        const auto current_idx  = static_cast<size_t>(std::distance(current_permutation.cbegin(), it));
        const auto source_coord = current_coords[current_idx];

        // Target x-coordinate is the x-coordinate of the i-th physical slot.
        const auto target_x = target_slots[i].x;

        distances[i] =
            static_cast<uint32_t>(std::abs(static_cast<int32_t>(source_coord.x) - static_cast<int32_t>(target_x)));
    }

    return distances;
}
/**
 * Calculates the number of rows required to route the pins from their current locations to the desired permutation
 * slots. Assumes pointy-top hexagonal layout constraints where horizontal movement requires vertical steps (2 rows per
 * 1 column shift).
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @tparam Pin Pin handle type.
 * @param current_permutation The current order of pins.
 * @param desired_permutation The desired order of pins.
 * @return The maximum number of rows required.
 */
template <typename Lyt, typename Pin>
uint32_t calculate_rows_needed(const Lyt& lyt, const std::vector<Pin>& current_permutation,
                               const std::vector<Pin>& desired_permutation)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    if (desired_permutation.empty() || current_permutation.empty())
    {
        return 0;
    }

    const auto distances = calculate_permutation_distances(lyt, current_permutation, desired_permutation);

    uint32_t max_rows = 0;

    for (const auto dist : distances)
    {
        // Hexagonal constraint: 2 rows per 1 unit of horizontal distance.
        const uint32_t rows = dist * 2;

        max_rows = std::max(rows, max_rows);
    }

    // Ensure that vertical offsets preserve hex row-shift parity.
    if (max_rows % 2 != 0)
    {
        ++max_rows;
    }

    return max_rows;
}
/**
 * Creates a new layout with space reserved for PI/PO unscrambling rows.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The original layout.
 * @param pi_rows Rows reserved for input unscrambling.
 * @param po_rows Rows reserved for output unscrambling.
 * @return New layout with extended height and original clocking/name.
 */
template <typename Lyt>
Lyt create_extended_layout(const Lyt& lyt, const uint32_t pi_rows, const uint32_t po_rows)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    const auto              crossing_layer = std::max(lyt.z(), static_cast<decltype(lyt.z())>(1));
    const aspect_ratio<Lyt> new_ar{lyt.x(), lyt.y() + pi_rows + po_rows, crossing_layer};
    return Lyt{new_ar, lyt.get_clocking_scheme(), lyt.get_layout_name()};
}
/**
 * Maps a primary input in the original layout to its routed anchor signal in the working layout.
 *
 * @tparam OrigLyt Original gate-level layout type.
 * @tparam WorkLyt Working gate-level layout type.
 */
template <typename OrigLyt, typename WorkLyt>
struct pi_anchor_signal
{
    /**
     * PI node in the original layout.
     */
    mockturtle::node<OrigLyt> original_pi{};
    /**
     * Routed anchor signal in the working layout.
     */
    mockturtle::signal<WorkLyt> routed_anchor{};
};
/**
 * Copies all internal nodes (gates and wires) from the original layout to the new layout with a vertical offset.
 * Primary inputs and outputs are intentionally omitted.
 *
 * @tparam SrcLyt Source gate-level layout type.
 * @tparam DstLyt Target gate-level layout type.
 * @param original_lyt The original layout to copy from.
 * @param target_lyt The target layout to copy to (must have sufficient space).
 * @param y_offset The vertical offset (in rows) to shift all coordinates by.
 */
template <typename SrcLyt, typename DstLyt>
void copy_layout_with_offset(const SrcLyt& original_lyt, DstLyt& target_lyt, const uint32_t y_offset,
                             const std::vector<pi_anchor_signal<SrcLyt, DstLyt>>& pi_anchors = {})
{
    static_assert(is_gate_level_layout_v<SrcLyt>, "SrcLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<SrcLyt>, "SrcLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<SrcLyt>, "SrcLyt does not have pointy-top hexagonal orientation");
    static_assert(is_gate_level_layout_v<DstLyt>, "DstLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<DstLyt>, "DstLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<DstLyt>, "DstLyt does not have pointy-top hexagonal orientation");

    // Map from original nodes to copied signals in the target layout
    mockturtle::node_map<mockturtle::signal<DstLyt>, SrcLyt> old2new{original_lyt};

    // Pre-map original PIs to their routed anchors that already exist in the target layout.
    original_lyt.foreach_pi(
        [&](const auto& pi)
        {
            const auto pi_anchor_it = std::find_if(pi_anchors.cbegin(), pi_anchors.cend(),
                                                   [&pi](const auto& anchor) { return anchor.original_pi == pi; });

            if (pi_anchor_it != pi_anchors.cend())
            {
                old2new[pi] = pi_anchor_it->routed_anchor;
                return;
            }

            const auto         pi_coord = original_lyt.get_tile(pi);
            const tile<DstLyt> shifted_pi_coord{pi_coord.x, pi_coord.y + y_offset, pi_coord.z};
            old2new[pi] = target_lyt.make_signal(target_lyt.get_node(shifted_pi_coord));
        });

    // Instantiate all non-PI/non-PO nodes at shifted coordinates with their actual copied fanins.
    original_lyt.foreach_gate(
        [&](const auto& node)
        {
            if (original_lyt.is_po(node))
            {
                return;
            }

            const auto         original_coord = original_lyt.get_tile(node);
            const tile<DstLyt> shifted_coord{original_coord.x, original_coord.y + y_offset, original_coord.z};

            std::vector<mockturtle::signal<DstLyt>> new_children{};
            new_children.reserve(original_lyt.fanin_size(node));
            auto fanin_collector = [&](const auto& fanin_signal)
            {
                const auto fanin_node = original_lyt.get_node(fanin_signal);
                auto       new_signal = old2new[fanin_node];
                const auto output_pin = fanin_signal.output;

                if (original_lyt.is_complemented(fanin_signal))
                {
                    new_signal = !new_signal;
                }

                new_signal.output = output_pin;
                new_children.push_back(new_signal);
            };
            original_lyt.template foreach_fanin<decltype(fanin_collector), false>(node, std::move(fanin_collector));

            assert(new_children.size() == original_lyt.fanin_size(node) && "Not all fanins were copied for a node");

            if constexpr (mockturtle::has_is_multioutput_v<SrcLyt> && mockturtle::has_node_function_pin_v<SrcLyt>)
            {
                if (original_lyt.is_multioutput(node))
                {
                    const auto num_outputs = [&original_lyt, &node]()
                    {
                        if constexpr (mockturtle::has_num_outputs_v<SrcLyt>)
                        {
                            return original_lyt.num_outputs(node);
                        }

                        return 2u;
                    }();

                    std::vector<kitty::dynamic_truth_table> functions{};
                    functions.reserve(num_outputs);

                    for (uint32_t pin = 0u; pin < num_outputs; ++pin)
                    {
                        functions.push_back(original_lyt.node_function_pin(node, pin));
                    }

                    old2new[node] = target_lyt.create_node(new_children, functions, shifted_coord);
                    return;
                }
            }

            old2new[node] = target_lyt.create_node(new_children, original_lyt.node_function(node), shifted_coord);
        });

    // Normalize copied fanins to eliminate temporary placeholders introduced during incremental mapping.
    original_lyt.foreach_gate(
        [&](const auto& node)
        {
            if (original_lyt.is_po(node))
            {
                return;
            }

            const auto         original_coord = original_lyt.get_tile(node);
            const tile<DstLyt> shifted_coord{original_coord.x, original_coord.y + y_offset, original_coord.z};

            std::vector<mockturtle::signal<DstLyt>> normalized_children{};
            normalized_children.reserve(original_lyt.fanin_size(node));
            auto normalized_fanin_collector = [&](const auto& fanin_signal)
            {
                const auto fanin_node = original_lyt.get_node(fanin_signal);
                auto       new_signal = old2new[fanin_node];
                const auto output_pin = fanin_signal.output;

                if (original_lyt.is_complemented(fanin_signal))
                {
                    new_signal = !new_signal;
                }

                new_signal.output = output_pin;
                normalized_children.push_back(new_signal);
            };
            original_lyt.template foreach_fanin<decltype(normalized_fanin_collector), false>(
                node, std::move(normalized_fanin_collector));

            target_lyt.move_node(target_lyt.get_node(old2new[node]), shifted_coord, normalized_children);
        });

    // Original POs are intentionally not copied and will be recreated later.
}
/**
 * Routing objective bundle for input unscrambling.
 *
 * @tparam OrigLyt Original gate-level layout type.
 * @tparam WorkLyt Working gate-level layout type.
 */
template <typename OrigLyt, typename WorkLyt>
struct pi_routing_objective
{
    /**
     * Horizontal routing distance used for objective prioritization.
     */
    uint32_t distance{};
    /**
     * Geometric source/target routing objective.
     */
    routing_objective<WorkLyt> objective{};
    /**
     * PI node in the original layout represented by this objective.
     */
    mockturtle::node<OrigLyt> original_pi{};
};
/**
 * Places new PIs in the top row and creates routing objectives to their original locations (shifted by pi_rows). The
 * objectives are sorted by their horizontal permutation distance to prioritize longer routes first.
 *
 * @tparam OrigLyt Original gate-level layout type.
 * @tparam WorkLyt Working gate-level layout type.
 * @param lyt The original layout.
 * @param new_layout The new layout to place PIs in.
 * @param current_pis The current PI ordering.
 * @param desired_pis The desired PI ordering.
 * @param pi_rows Rows reserved for input unscrambling.
 * @return Routing objectives sorted by permutation distance.
 */
template <typename OrigLyt, typename WorkLyt>
std::vector<pi_routing_objective<OrigLyt, WorkLyt>>
create_pi_routing_objectives(const OrigLyt& lyt, WorkLyt& new_layout,
                             const std::vector<mockturtle::node<OrigLyt>>& current_pis,
                             const std::vector<mockturtle::node<OrigLyt>>& desired_pis, const uint32_t pi_rows)
{
    static_assert(is_gate_level_layout_v<OrigLyt>, "OrigLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<OrigLyt>, "OrigLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<OrigLyt>, "OrigLyt does not have pointy-top hexagonal orientation");
    static_assert(is_gate_level_layout_v<WorkLyt>, "WorkLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<WorkLyt>, "WorkLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<WorkLyt>, "WorkLyt does not have pointy-top hexagonal orientation");

    if (desired_pis.empty())
    {
        return {};
    }

    // Current PI coordinates define the available physical interface slots.
    const auto current_pi_coords = determine_distributed_pin_slot_coordinates(lyt, current_pis);
    // Precompute horizontal distances to sort objectives by route length.
    const auto pi_distances = calculate_permutation_distances(lyt, current_pis, desired_pis);

    std::vector<pi_routing_objective<OrigLyt, WorkLyt>> pi_objectives{};
    pi_objectives.reserve(desired_pis.size());

    for (size_t i = 0; i < desired_pis.size(); ++i)
    {
        if (i >= current_pi_coords.size())
        {
            break;
        }

        const auto desired_node = desired_pis[i];
        const auto slot_x       = current_pi_coords[i].x;

        // Place new PI in the top row of the new layout at the desired x-coordinate.
        const tile<WorkLyt> source{slot_x, 0};
        const auto          pi_name = lyt.get_name(desired_node);
        new_layout.create_pi(pi_name, source);

        // Route to the original PI coordinate, shifted down by the PI routing rows.
        const auto          original_coord = lyt.get_tile(desired_node);
        const tile<WorkLyt> target{original_coord.x, original_coord.y + pi_rows, original_coord.z};

        pi_objectives.push_back({pi_distances[i], {source, target}, desired_node});
    }

    // Sort objectives by distance (descending) to prioritize longer routes first.
    std::sort(pi_objectives.begin(), pi_objectives.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.distance > rhs.distance; });

    return pi_objectives;
}
/**
 * Routes PI unscrambling objectives sequentially using A* with crossings enabled.
 *
 * @tparam OrigLyt Original gate-level layout type.
 * @tparam WorkLyt Working gate-level layout type.
 * @param lyt Layout to route on.
 * @param objectives PI routing objectives in priority order.
 * @return Routed anchor signals for original PIs.
 */
template <typename OrigLyt, typename WorkLyt>
std::vector<pi_anchor_signal<OrigLyt, WorkLyt>>
route_pi_objectives_with_a_star(WorkLyt& lyt, const std::vector<pi_routing_objective<OrigLyt, WorkLyt>>& objectives)
{
    static_assert(is_gate_level_layout_v<OrigLyt>, "OrigLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<OrigLyt>, "OrigLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<OrigLyt>, "OrigLyt does not have pointy-top hexagonal orientation");
    static_assert(is_gate_level_layout_v<WorkLyt>, "WorkLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<WorkLyt>, "WorkLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<WorkLyt>, "WorkLyt does not have pointy-top hexagonal orientation");

    a_star_params params{};
    params.crossings = true;

    std::vector<pi_anchor_signal<OrigLyt, WorkLyt>> routed_anchors{};
    routed_anchors.reserve(objectives.size());

    for (const auto& item : objectives)
    {
        const auto path = a_star<layout_coordinate_path<WorkLyt>>(lyt, {item.objective.source, item.objective.target},
                                                                  euclidean_distance_functor<WorkLyt>(),
                                                                  unit_cost_functor<WorkLyt>(), params);

        if (path.empty())
        {
            throw std::runtime_error(fmt::format(
                "A* failed to route PI unscrambling objective from ({}, {}, {}) to ({}, {}, {}).",
                static_cast<uint64_t>(item.objective.source.x), static_cast<uint64_t>(item.objective.source.y),
                static_cast<uint64_t>(item.objective.source.z), static_cast<uint64_t>(item.objective.target.x),
                static_cast<uint64_t>(item.objective.target.y), static_cast<uint64_t>(item.objective.target.z)));
        }

        auto incoming_signal = lyt.make_signal(lyt.get_node(path.source()));

        // Materialize the entire PI route including the target tile to create a concrete anchor node at target.
        std::for_each(path.cbegin() + 1, path.cend(),
                      [&](const auto& coord)
                      {
                          incoming_signal =
                              lyt.create_buf(incoming_signal, lyt.is_empty_tile(coord) ? coord : lyt.above(coord));
                      });

        routed_anchors.push_back({item.original_pi, incoming_signal});
    }

    return routed_anchors;
}
/**
 * Routing objective bundle for output unscrambling.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct po_routing_objective
{
    /**
     * Horizontal routing distance used for objective prioritization.
     */
    uint32_t distance{};
    /**
     * Geometric source/target routing objective.
     */
    routing_objective<Lyt> objective{};
    /**
     * Driver signal of the desired PO in the copied logic.
     */
    mockturtle::signal<Lyt> source_driver{};
    /**
     * Output name associated with the routing objective.
     */
    std::string output_name{};
    /**
     * Target PO position in the requested output ordering.
     */
    uint32_t order_index{};
    /**
     * Indicates whether geometric routing is required before PO creation.
     */
    bool requires_routing{true};
};
/**
 * Places output source anchors at shifted original PO locations and creates routing objectives towards new PO slots in
 * the bottom extension area.
 *
 * @tparam OrigLyt Original gate-level layout type.
 * @tparam WorkLyt Working gate-level layout type.
 * @param lyt Original layout.
 * @param new_layout Extended/copied layout.
 * @param current_pos Current PO ordering.
 * @param desired_pos Desired PO ordering.
 * @param pi_rows Rows reserved for input unscrambling (vertical offset of copied logic).
 * @param po_rows Rows reserved for output unscrambling.
 * @return Output routing objectives with associated output names.
 */
template <typename OrigLyt, typename WorkLyt>
std::vector<po_routing_objective<WorkLyt>> create_po_routing_objectives(
    const OrigLyt& lyt, WorkLyt& new_layout, const std::vector<mockturtle::signal<OrigLyt>>& current_pos,
    const std::vector<mockturtle::signal<OrigLyt>>& desired_pos, const uint32_t pi_rows, const uint32_t po_rows)
{
    static_assert(is_gate_level_layout_v<OrigLyt>, "OrigLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<OrigLyt>, "OrigLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<OrigLyt>, "OrigLyt does not have pointy-top hexagonal orientation");
    static_assert(is_gate_level_layout_v<WorkLyt>, "WorkLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<WorkLyt>, "WorkLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<WorkLyt>, "WorkLyt does not have pointy-top hexagonal orientation");

    std::vector<po_routing_objective<WorkLyt>> po_objectives{};

    if (desired_pos.empty() || current_pos.empty())
    {
        return {};
    }

    const auto current_po_coords = determine_distributed_pin_slot_coordinates(lyt, current_pos);

    po_objectives.reserve(desired_pos.size());

    for (size_t i = 0; i < desired_pos.size(); ++i)
    {
        const auto desired_po_signal = desired_pos[i];

        const auto current_it = std::find(current_pos.cbegin(), current_pos.cend(), desired_po_signal);
        if (current_it == current_pos.cend())
        {
            continue;
        }

        const auto current_idx = static_cast<size_t>(std::distance(current_pos.cbegin(), current_it));

        const auto original_po_signal = lyt.po_at(static_cast<uint32_t>(current_idx));
        const auto original_po_tile   = static_cast<tile<OrigLyt>>(original_po_signal);

        tile<WorkLyt> source_tile{original_po_tile.x, original_po_tile.y + pi_rows, original_po_tile.z};

        const auto    target_x = current_po_coords[i].x;
        tile<WorkLyt> target_tile{target_x, current_po_coords[i].y + pi_rows + po_rows, current_po_coords[i].z};

        mockturtle::signal<WorkLyt> source_driver{};

        auto fanin_collector = [&](const auto& fanin_signal)
        {
            const auto fanin_node = lyt.get_node(fanin_signal);
            const auto fanin_tile = lyt.get_tile(fanin_node);

            source_driver = static_cast<mockturtle::signal<WorkLyt>>(
                tile<WorkLyt>{fanin_tile.x, fanin_tile.y + pi_rows, fanin_tile.z});
            const auto output_pin = fanin_signal.output;

            if (lyt.is_complemented(fanin_signal))
            {
                source_driver = !source_driver;
            }

            source_driver.output = output_pin;

            return;
        };

        lyt.template foreach_fanin<decltype(fanin_collector), false>(lyt.get_node(desired_po_signal),
                                                                     std::move(fanin_collector));

        const auto output_name = lyt.get_output_name(static_cast<uint32_t>(current_idx));

        const auto requires_routing = source_tile != target_tile;

        if (requires_routing && new_layout.is_empty_tile(source_tile))
        {
            new_layout.create_buf(source_driver, source_tile);
        }

        const auto distance =
            static_cast<uint32_t>(std::abs(static_cast<int32_t>(source_tile.x) - static_cast<int32_t>(target_tile.x)));

        po_objectives.push_back({distance,
                                 {source_tile, target_tile},
                                 source_driver,
                                 output_name,
                                 static_cast<uint32_t>(i),
                                 requires_routing});
    }

    std::sort(po_objectives.begin(), po_objectives.end(),
              [](const auto& lhs, const auto& rhs)
              {
                  if (lhs.distance != rhs.distance)
                  {
                      return lhs.distance > rhs.distance;
                  }

                  if (lhs.objective.source.x != rhs.objective.source.x)
                  {
                      return lhs.objective.source.x < rhs.objective.source.x;
                  }

                  return lhs.order_index < rhs.order_index;
              });

    return po_objectives;
}
/**
 * Routes output objectives sequentially and creates new POs at target coordinates.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to route on.
 * @param objectives Output routing objectives with output names.
 */
template <typename Lyt>
void route_po_objectives_with_a_star_and_create_pos(Lyt& lyt, const std::vector<po_routing_objective<Lyt>>& objectives)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    a_star_params params{};
    params.crossings = true;

    struct routed_po
    {
        mockturtle::signal<Lyt> driver{};
        tile<Lyt>               target{};
        std::string             output_name{};
        uint32_t                order_index{};
    };

    std::vector<routed_po> routed_pos{};
    routed_pos.reserve(objectives.size());

    for (const auto& item : objectives)
    {
        auto final_driver = item.source_driver;

        if (item.requires_routing)
        {
            const auto path = a_star<layout_coordinate_path<Lyt>>(lyt, {item.objective.source, item.objective.target},
                                                                  euclidean_distance_functor<Lyt>(),
                                                                  unit_cost_functor<Lyt>(), params);

            if (path.empty())
            {
                throw std::runtime_error(fmt::format(
                    "A* failed to route PO unscrambling objective from ({}, {}, {}) to ({}, {}, {}).",
                    static_cast<uint64_t>(item.objective.source.x), static_cast<uint64_t>(item.objective.source.y),
                    static_cast<uint64_t>(item.objective.source.z), static_cast<uint64_t>(item.objective.target.x),
                    static_cast<uint64_t>(item.objective.target.y), static_cast<uint64_t>(item.objective.target.z)));
            }

            auto incoming_signal = lyt.make_signal(lyt.get_node(path.source()));

            std::for_each(path.cbegin() + 1, path.cend() - 1,
                          [&](const auto& coord)
                          {
                              incoming_signal =
                                  lyt.create_buf(incoming_signal, lyt.is_empty_tile(coord) ? coord : lyt.above(coord));
                          });

            final_driver = incoming_signal;
        }

        lyt.create_po(final_driver, item.output_name, item.objective.target);
        routed_pos.push_back({final_driver, item.objective.target, item.output_name, item.order_index});
    }

    std::vector<tile<Lyt>> current_po_tiles{};
    current_po_tiles.reserve(lyt.num_pos());

    lyt.foreach_po([&current_po_tiles](const auto& po) { current_po_tiles.push_back(static_cast<tile<Lyt>>(po)); });

    for (const auto& po_tile : current_po_tiles)
    {
        lyt.clear_tile(po_tile);
    }

    std::sort(routed_pos.begin(), routed_pos.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.order_index < rhs.order_index; });

    for (const auto& po : routed_pos)
    {
        lyt.create_po(po.driver, po.output_name, po.target);
    }
}

template <typename Lyt>
class unscramble_pins_impl
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

  public:
    unscramble_pins_impl(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& input_order,
                         const std::vector<mockturtle::signal<Lyt>>& output_order, const unscramble_pins_params& p,
                         unscramble_pins_stats& st) :
            layout{lyt},
            input_ordering{input_order},
            output_ordering{output_order},
            params{p},
            pst{st}
    {}

    Lyt run()
    {
        mockturtle::stopwatch stop{pst.time_total};

        debug::write_dot_layout<Lyt, gate_layout_hexagonal_drawer<Lyt>>(static_cast<Lyt>(layout),
                                                                        "layout_before_pin_unscrambling");

        // 1. Identify current pin orderings
        const auto current_pis = get_current_pis();
        const auto current_pos = get_current_pos();

        // Use identity permutation when no explicit target ordering was provided.
        const auto& target_pis = input_ordering.empty() ? current_pis : input_ordering;
        const auto& target_pos = output_ordering.empty() ? current_pos : output_ordering;

        // 2. Calculate unscrambling space requirements
        uint32_t pi_rows = calculate_rows_needed(layout, current_pis, target_pis);
        uint32_t po_rows = calculate_rows_needed(layout, current_pos, target_pos);

        const auto num_pis = static_cast<uint32_t>(layout.num_pis());
        const auto num_pos = static_cast<uint32_t>(layout.num_pos());

        const auto max_pi_rows = pi_rows + std::max<uint32_t>(2u, num_pis * 4u);
        const auto max_po_rows = po_rows + std::max<uint32_t>(2u, num_pos * 4u);

        while (true)
        {
            try
            {
                return run_with_reserved_rows(current_pis, target_pis, current_pos, target_pos, pi_rows, po_rows);
            }
            catch (const std::runtime_error& e)
            {
                const std::string_view message{e.what()};

                if (message.find("A* failed to route PI unscrambling objective") == 0u && pi_rows < max_pi_rows)
                {
                    pi_rows += 2u;
                    continue;
                }

                if (message.find("A* failed to route PO unscrambling objective") == 0u && po_rows < max_po_rows)
                {
                    po_rows += 2u;
                    continue;
                }

                throw;
            }
        }
    }

  private:
    /**
     * The original layout to unscramble.
     */
    const Lyt& layout;
    /**
     * The desired ordering of primary input nodes.
     */
    std::vector<mockturtle::node<Lyt>> input_ordering;
    /**
     * The desired ordering of primary output signals.
     */
    std::vector<mockturtle::signal<Lyt>> output_ordering;
    /**
     * Parameters for the pin unscrambling algorithm.
     */
    unscramble_pins_params params;
    /**
     * Statistics for the pin unscrambling algorithm.
     */
    unscramble_pins_stats& pst;

    /**
     * Identifies the current primary input nodes in the layout.
     *
     * @return Vector of primary input nodes.
     */
    std::vector<mockturtle::node<Lyt>> get_current_pis() const
    {
        std::vector<mockturtle::node<Lyt>> pis{};
        pis.reserve(layout.num_pis());
        layout.foreach_pi([&pis](const auto& pi) { pis.push_back(pi); });

        return pis;
    }
    /**
     * Identifies the current primary output signals in the layout.
     *
     * @return Vector of primary output signals.
     */
    std::vector<mockturtle::signal<Lyt>> get_current_pos() const
    {
        std::vector<mockturtle::signal<Lyt>> pos{};
        pos.reserve(layout.num_pos());
        layout.foreach_po([&pos](const auto& po) { pos.push_back(po); });

        return pos;
    }

    /**
     * Performs one unscrambling attempt with explicitly reserved PI/PO routing rows.
     *
     * @param current_pis Current PI ordering.
     * @param target_pis Target PI ordering.
     * @param current_pos Current PO ordering.
     * @param target_pos Target PO ordering.
     * @param pi_rows Rows reserved for input unscrambling.
     * @param po_rows Rows reserved for output unscrambling.
     * @return Unscrambled layout.
     */
    [[nodiscard]] Lyt run_with_reserved_rows(const std::vector<mockturtle::node<Lyt>>&   current_pis,
                                             const std::vector<mockturtle::node<Lyt>>&   target_pis,
                                             const std::vector<mockturtle::signal<Lyt>>& current_pos,
                                             const std::vector<mockturtle::signal<Lyt>>& target_pos,
                                             const uint32_t pi_rows, const uint32_t po_rows) const
    {
        std::string last_pi_routing_error{};

        for (uint8_t pi_order_strategy = 0u; pi_order_strategy < 6u; ++pi_order_strategy)
        {
            // 3. Instantiate new obstruction-aware layout with extended height
            obstruction_layout<Lyt> new_layout{create_extended_layout(layout, pi_rows, po_rows)};

            // 4. Place new PIs and create routing objectives to the original PI locations (shifted by pi_rows)
            auto pi_routing_objectives =
                create_pi_routing_objectives(layout, new_layout, current_pis, target_pis, pi_rows);

            switch (pi_order_strategy)
            {
                case 0u: break;
                case 1u:
                    std::sort(pi_routing_objectives.begin(), pi_routing_objectives.end(),
                              [](const auto& lhs, const auto& rhs) { return lhs.distance < rhs.distance; });
                    break;
                case 2u:
                    std::sort(pi_routing_objectives.begin(), pi_routing_objectives.end(),
                              [](const auto& lhs, const auto& rhs)
                              {
                                  if (lhs.objective.source.x != rhs.objective.source.x)
                                  {
                                      return lhs.objective.source.x < rhs.objective.source.x;
                                  }

                                  return lhs.objective.target.x < rhs.objective.target.x;
                              });
                    break;
                case 3u:
                    std::sort(pi_routing_objectives.begin(), pi_routing_objectives.end(),
                              [](const auto& lhs, const auto& rhs)
                              {
                                  if (lhs.objective.source.x != rhs.objective.source.x)
                                  {
                                      return lhs.objective.source.x > rhs.objective.source.x;
                                  }

                                  return lhs.objective.target.x > rhs.objective.target.x;
                              });
                    break;
                case 4u:
                    std::sort(pi_routing_objectives.begin(), pi_routing_objectives.end(),
                              [](const auto& lhs, const auto& rhs)
                              {
                                  if (lhs.objective.target.x != rhs.objective.target.x)
                                  {
                                      return lhs.objective.target.x < rhs.objective.target.x;
                                  }

                                  return lhs.objective.source.x < rhs.objective.source.x;
                              });
                    break;
                case 5u:
                    std::sort(pi_routing_objectives.begin(), pi_routing_objectives.end(),
                              [](const auto& lhs, const auto& rhs)
                              {
                                  if (lhs.objective.target.x != rhs.objective.target.x)
                                  {
                                      return lhs.objective.target.x > rhs.objective.target.x;
                                  }

                                  return lhs.objective.source.x > rhs.objective.source.x;
                              });
                    break;
                default: break;
            }

            try
            {
                // 5. Route PI objectives in priority order
                const auto pi_anchor_signals = route_pi_objectives_with_a_star(new_layout, pi_routing_objectives);

                // 6. Copy the original layout content to the new layout with vertical offset
                copy_layout_with_offset(layout, new_layout, pi_rows, pi_anchor_signals);

                debug::write_dot_layout<Lyt, gate_layout_hexagonal_drawer<Lyt>>(static_cast<Lyt>(new_layout),
                                                                                "unscramble_pins_after_copy");

                // 7. Place output source anchors, route to new output slots, and create new POs.
                const auto po_routing_objectives =
                    create_po_routing_objectives(layout, new_layout, current_pos, target_pos, pi_rows, po_rows);

                // 8. Route PO objectives and create POs at target locations.
                route_po_objectives_with_a_star_and_create_pos(new_layout, po_routing_objectives);

                // Temporary debug artifact for inspecting the fully unscrambled layout.
                debug::write_dot_layout<Lyt, gate_layout_hexagonal_drawer<Lyt>>(static_cast<Lyt>(new_layout),
                                                                                "unscramble_pins_full_after_routing");

                return static_cast<Lyt>(new_layout);
            }
            catch (const std::runtime_error& e)
            {
                const std::string_view message{e.what()};

                if (message.find("A* failed to route PI unscrambling objective") == 0u)
                {
                    last_pi_routing_error = e.what();
                    continue;
                }

                throw;
            }
        }

        throw std::runtime_error{last_pi_routing_error.empty() ? "PI unscrambling failed." : last_pi_routing_error};
    }
};

}  // namespace detail

/**
 * A physical design algorithm to unscramble input and output pin orderings in placed and routed pointy-top row-wise
 * clocked hexagonal gate-level layouts.
 *
 * This algorithm takes an existing gate-level layout and reorders its primary inputs and outputs according to a
 * specified target ordering.
 *
 * @note This significantly increases the area footprint of the layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The gate-level layout to unscramble.
 * @param input_order The desired ordering of primary input nodes.
 * @param output_order The desired ordering of primary output signals.
 * @param ps Parameters for the algorithm.
 * @param pst Statistics for the algorithm.
 * @return A new gate-level layout with unscrambled pins.
 */
template <typename Lyt>
Lyt unscramble_pins(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& input_order,
                    const std::vector<mockturtle::signal<Lyt>>& output_order, unscramble_pins_params ps = {},
                    unscramble_pins_stats* pst = nullptr)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    assert(lyt.is_clocking_scheme("ROW") && "Layout must be row-wise clocked");

    unscramble_pins_stats             st{};
    detail::unscramble_pins_impl<Lyt> p{lyt, input_order, output_order, ps, st};

    const auto result = p.run();

    if (pst)
    {
        *pst = st;
    }

    return result;
}

/**
 * A compatibility overload that resolves primary outputs by node identity.
 *
 * This overload cannot express distinct permutations of multiple POs that originate from the same multi-output gate.
 * Prefer the signal-based overload above whenever exact PO output-pin identity matters.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The gate-level layout to unscramble.
 * @param input_order The desired ordering of primary input nodes.
 * @param output_order The desired ordering of primary output nodes.
 * @param ps Parameters for the algorithm.
 * @param pst Statistics for the algorithm.
 * @return A new gate-level layout with unscrambled pins.
 */
template <typename Lyt>
Lyt unscramble_pins(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& input_order,
                    const std::vector<mockturtle::node<Lyt>>& output_order, unscramble_pins_params ps,
                    unscramble_pins_stats* pst)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    std::vector<mockturtle::signal<Lyt>> resolved_output_order{};
    resolved_output_order.reserve(output_order.size());

    std::vector<bool> consumed_pos(lyt.num_pos(), false);

    for (const auto desired_po_node : output_order)
    {
        auto matched = false;

        for (uint32_t index = 0u; index < lyt.num_pos(); ++index)
        {
            if (consumed_pos[index])
            {
                continue;
            }

            const auto po_signal = lyt.po_at(index);
            if (lyt.get_node(po_signal) != desired_po_node)
            {
                continue;
            }

            resolved_output_order.push_back(po_signal);
            consumed_pos[index] = true;
            matched             = true;
            break;
        }

        if (!matched)
        {
            throw std::invalid_argument("Unable to resolve PO node ordering to concrete output signals.");
        }
    }

    return unscramble_pins(lyt, input_order, resolved_output_order, ps, pst);
}

}  // namespace fiction

#endif  // FICTION_UNSCRAMBLE_PINS_HPP
