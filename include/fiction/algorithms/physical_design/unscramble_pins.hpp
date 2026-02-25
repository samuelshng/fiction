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
#include <string>
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
 * Extracts the layout coordinates for a given list of pin nodes.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @param pins Vector of pin nodes.
 * @return Vector of coordinates corresponding to the pins.
 */
template <typename Lyt>
std::vector<tile<Lyt>> determine_pin_coordinates(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& pins)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    std::vector<tile<Lyt>> coords{};
    coords.reserve(pins.size());
    for (const auto& pin : pins)
    {
        coords.push_back(lyt.get_tile(pin));
    }

    return coords;
}
/**
 * Calculates the horizontal permutation distances for each desired slot.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @param current_permutation The current order of pins (nodes).
 * @param desired_permutation The desired order of pins (nodes).
 * @return Vector of horizontal distances per desired slot.
 */
template <typename Lyt>
std::vector<uint32_t> calculate_permutation_distances(const Lyt&                                lyt,
                                                      const std::vector<mockturtle::node<Lyt>>& current_permutation,
                                                      const std::vector<mockturtle::node<Lyt>>& desired_permutation)
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

        // Target x-coordinate is the x-coordinate of the i-th slot (from current_coords[i])
        const auto target_x = current_coords[i].x;

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
 * @param current_permutation The current order of pins (nodes).
 * @param desired_permutation The desired order of pins (nodes).
 * @return The maximum number of rows required.
 */
template <typename Lyt>
uint32_t calculate_rows_needed(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& current_permutation,
                               const std::vector<mockturtle::node<Lyt>>& desired_permutation)
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
void copy_layout_with_offset(const SrcLyt& original_lyt, DstLyt& target_lyt, const uint32_t y_offset)
{
    static_assert(is_gate_level_layout_v<SrcLyt>, "SrcLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<SrcLyt>, "SrcLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<SrcLyt>, "SrcLyt does not have pointy-top hexagonal orientation");
    static_assert(is_gate_level_layout_v<DstLyt>, "DstLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<DstLyt>, "DstLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<DstLyt>, "DstLyt does not have pointy-top hexagonal orientation");

    // Map from original nodes to copied signals in the target layout
    mockturtle::node_map<mockturtle::signal<DstLyt>, SrcLyt> old2new{original_lyt};

    // Step 1: Instantiate all non-PI/non-PO nodes at shifted coordinates with temporary children.
    original_lyt.foreach_gate(
        [&](const auto& node)
        {
            if (original_lyt.is_po(node))
            {
                return;
            }

            const auto         original_coord = original_lyt.get_tile(node);
            const tile<DstLyt> shifted_coord{original_coord.x, original_coord.y + y_offset, original_coord.z};

            std::vector<mockturtle::signal<DstLyt>> temporary_children(original_lyt.fanin_size(node),
                                                                       mockturtle::signal<DstLyt>{});

            old2new[node] = target_lyt.create_node(temporary_children, original_lyt.node_function(node), shifted_coord);
        });

    // Step 2: Rebind all copied nodes to their actual fanins while preserving edge polarity/output pins.
    original_lyt.foreach_gate(
        [&](const auto& node)
        {
            if (original_lyt.is_po(node))
            {
                return;
            }

            // Collect copied fanins and preserve complemented edges.
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

            const auto         original_coord = original_lyt.get_tile(node);
            const tile<DstLyt> shifted_coord{original_coord.x, original_coord.y + y_offset, original_coord.z};
            target_lyt.move_node(target_lyt.get_node(old2new[node]), shifted_coord, new_children);
        });

    // Step 3: Original POs are intentionally not copied and will be recreated later.
}
/**
 * Routing objective bundle for input unscrambling.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct pi_routing_objective
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
     * Shifted tile of the copied fanout node that must be connected to the routed PI path.
     */
    tile<Lyt> fanout_target{};
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
std::vector<pi_routing_objective<WorkLyt>>
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

    // Current PI coordinates define the slot x-positions for the desired ordering.
    const auto current_pi_coords = determine_pin_coordinates(lyt, current_pis);
    // Precompute horizontal distances to sort objectives by route length.
    const auto pi_distances = calculate_permutation_distances(lyt, current_pis, desired_pis);

    std::vector<pi_routing_objective<WorkLyt>> pi_objectives{};
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

        tile<WorkLyt> fanout_target{};

        lyt.foreach_fanout(desired_node,
                           [&](const auto& fanout)
                           {
                               const auto fanout_coord = lyt.get_tile(fanout);
                               fanout_target = tile<WorkLyt>{fanout_coord.x, fanout_coord.y + pi_rows, fanout_coord.z};
                               return;
                           });

        pi_objectives.push_back({pi_distances[i], {source, target}, fanout_target});
    }

    // Sort objectives by distance (descending) to prioritize longer routes first.
    std::sort(pi_objectives.begin(), pi_objectives.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.distance > rhs.distance; });

    return pi_objectives;
}
/**
 * Routes PI unscrambling objectives sequentially using A* with crossings enabled.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to route on.
 * @param objectives PI routing objectives in priority order.
 */
template <typename Lyt>
void route_pi_objectives_with_a_star(Lyt& lyt, const std::vector<pi_routing_objective<Lyt>>& objectives)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    a_star_params params{};
    params.crossings = true;

    for (const auto& item : objectives)
    {
        auto incoming_signal = lyt.make_signal(lyt.get_node(item.objective.source));

        if (item.objective.source != item.objective.target)
        {
            const auto path = a_star<layout_coordinate_path<Lyt>>(lyt, {item.objective.source, item.objective.target},
                                                                  euclidean_distance_functor<Lyt>(),
                                                                  unit_cost_functor<Lyt>(), params);

            assert(!path.empty() && "A* failed to route a PI unscrambling objective");

            // Materialize the entire PI route including the target tile to create a concrete anchor node at target.
            std::for_each(path.cbegin() + 1, path.cend(),
                          [&](const auto& coord) { incoming_signal = lyt.create_buf(incoming_signal, coord); });
        }

        lyt.connect(incoming_signal, lyt.get_node(item.fanout_target));
    }
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
     * Output name associated with the routing objective.
     */
    std::string output_name{};
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
    const OrigLyt& lyt, WorkLyt& new_layout, const std::vector<mockturtle::node<OrigLyt>>& current_pos,
    const std::vector<mockturtle::node<OrigLyt>>& desired_pos, const uint32_t pi_rows, const uint32_t po_rows)
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

    const auto current_po_coords = determine_pin_coordinates(lyt, current_pos);

    po_objectives.reserve(desired_pos.size());

    for (size_t i = 0; i < desired_pos.size(); ++i)
    {
        const auto desired_po_node = desired_pos[i];

        const auto current_it = std::find(current_pos.cbegin(), current_pos.cend(), desired_po_node);
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
        bool                        has_fanin = false;

        auto fanin_collector = [&](const auto& fanin_signal)
        {
            if (has_fanin)
            {
                return;
            }

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

            has_fanin = true;
        };

        lyt.template foreach_fanin<decltype(fanin_collector), false>(desired_po_node, std::move(fanin_collector));

        if (!has_fanin)
        {
            source_driver = new_layout.get_constant(false);
        }

        const auto output_name = lyt.get_output_name(static_cast<uint32_t>(current_idx));

        // Identity case: no routing required; directly create PO at target.
        if (source_tile == target_tile)
        {
            new_layout.create_po(source_driver, output_name, target_tile);
            continue;
        }

        if (new_layout.is_empty_tile(source_tile))
        {
            new_layout.create_buf(source_driver, source_tile);
        }

        const auto distance =
            static_cast<uint32_t>(std::abs(static_cast<int32_t>(source_tile.x) - static_cast<int32_t>(target_tile.x)));

        po_objectives.push_back({distance, {source_tile, target_tile}, output_name});
    }

    std::sort(po_objectives.begin(), po_objectives.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.distance > rhs.distance; });

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

    for (const auto& item : objectives)
    {
        const auto path =
            a_star<layout_coordinate_path<Lyt>>(lyt, {item.objective.source, item.objective.target},
                                                euclidean_distance_functor<Lyt>(), unit_cost_functor<Lyt>(), params);

        assert(!path.empty() && "A* failed to route a PO unscrambling objective");

        auto incoming_signal = lyt.make_signal(lyt.get_node(path.source()));

        std::for_each(path.cbegin() + 1, path.cend() - 1,
                      [&](const auto& coord)
                      {
                          incoming_signal =
                              lyt.create_buf(incoming_signal, lyt.is_empty_tile(coord) ? coord : lyt.above(coord));
                      });

        lyt.create_po(incoming_signal, item.output_name, path.target());
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
                         const std::vector<mockturtle::node<Lyt>>& output_order, const unscramble_pins_params& p,
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
        const uint32_t pi_rows = calculate_rows_needed(layout, current_pis, target_pis);
        const uint32_t po_rows = calculate_rows_needed(layout, current_pos, target_pos);

        // 3. Instantiate new obstruction-aware layout with extended height
        obstruction_layout<Lyt> new_layout{create_extended_layout(layout, pi_rows, po_rows)};

        // 4. Place new PIs and create routing objectives to the original PI locations (shifted by pi_rows)
        const auto pi_routing_objectives =
            create_pi_routing_objectives(layout, new_layout, current_pis, target_pis, pi_rows);

        // 5. Copy the original layout content to the new layout with vertical offset
        copy_layout_with_offset(layout, new_layout, pi_rows);

        debug::write_dot_layout<Lyt, gate_layout_hexagonal_drawer<Lyt>>(static_cast<Lyt>(new_layout),
                                                                        "unscramble_pins_after_copy");

        // 6. Route PI objectives in priority order
        route_pi_objectives_with_a_star(new_layout, pi_routing_objectives);

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
     * The desired ordering of primary output nodes.
     */
    std::vector<mockturtle::node<Lyt>> output_ordering;
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
     * Identifies the current primary output nodes in the layout.
     *
     * @return Vector of primary output nodes.
     */
    std::vector<mockturtle::node<Lyt>> get_current_pos() const
    {
        std::vector<mockturtle::node<Lyt>> pos{};
        pos.reserve(layout.num_pos());
        layout.foreach_po([&pos, this](const auto& po) { pos.push_back(layout.get_node(po)); });

        return pos;
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
 * @param output_order The desired ordering of primary output nodes.
 * @param ps Parameters for the algorithm.
 * @param pst Statistics for the algorithm.
 * @return A new gate-level layout with unscrambled pins.
 */
template <typename Lyt>
Lyt unscramble_pins(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& input_order,
                    const std::vector<mockturtle::node<Lyt>>& output_order, unscramble_pins_params ps = {},
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

}  // namespace fiction

#endif  // FICTION_UNSCRAMBLE_PINS_HPP
