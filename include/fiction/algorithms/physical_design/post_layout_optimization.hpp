//
// Created by simon on 14.06.23.
//

#ifndef FICTION_POST_LAYOUT_OPTIMIZATION_HPP
#define FICTION_POST_LAYOUT_OPTIMIZATION_HPP

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"

#include "fiction/algorithms/path_finding/a_star.hpp"
#include "fiction/algorithms/path_finding/cost.hpp"
#include "fiction/algorithms/path_finding/distance.hpp"
#include "fiction/algorithms/physical_design/wiring_reduction.hpp"
#include "fiction/layouts/bounding_box.hpp"
#include "fiction/layouts/clocking_scheme.hpp"
#include "fiction/layouts/obstruction_layout.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/routing_utils.hpp"

#include <mockturtle/traits.hpp>
#include <mockturtle/utils/stopwatch.hpp>
#include <phmap.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

namespace fiction
{

/**
 * Parameters for the post-layout optimization algorithm.
 */
struct post_layout_optimization_params
{
    /**
     * Maximum number of relocations to try for each gate. Defaults to the number of tiles in the given layout if not
     * specified.
     */
    std::optional<uint64_t> max_gate_relocations = std::nullopt;
    /**
     * Only optimize PO positions.
     */
    bool optimize_pos_only = false;
    /**
     * Disable the creation of crossings during optimization. If set to true, gates will only be relocated if a
     * crossing-free wiring is found. Defaults to false.
     */
    bool planar_optimization = false;
    /**
     * Timeout limit (in ms). Specifies the maximum allowed time in milliseconds for the optimization process. For large
     * layouts, the actual execution time may slightly exceed this limit because it's impractical to check the timeout
     * at every algorithm step and the functional correctness has to be ensured by completing essential algorithm steps.
     */
    uint64_t timeout = std::numeric_limits<uint64_t>::max();
};

/**
 * This struct stores statistics about the post-layout optimization process.
 */
struct post_layout_optimization_stats
{
    /**
     * Runtime of the post-layout optimization process.
     */
    mockturtle::stopwatch<>::duration time_total{0};
    /**
     * Layout width before the post-layout optimization process.
     */
    uint64_t x_size_before{0ull};
    /**
     * Layout height before the post-layout optimization process.
     */
    uint64_t y_size_before{0ull};
    /**
     * Layout width after the post-layout optimization process.
     */
    uint64_t x_size_after{0ull};
    /**
     * Layout height after the post-layout optimization process.
     */
    uint64_t y_size_after{0ull};
    /**
     * Area reduction (in %) after the post-layout optimization process.
     */
    double_t area_improvement{0ull};
    /**
     * Number of wire segments before the post-layout optimization process.
     */
    uint64_t num_wires_before{0ull};
    /**
     * Number of wire segments after the post-layout optimization process.
     */
    uint64_t num_wires_after{0ull};
    /**
     * Number of crossings before the post-layout optimization process.
     */
    uint64_t num_crossings_before{0ull};
    /**
     * Number of crossings after the post-layout optimization process.
     */
    uint64_t num_crossings_after{0ull};
    /**
     * Reports the statistics to the given output stream.
     *
     * @param out Output stream.
     */
    void report(std::ostream& out = std::cout) const
    {
        out << fmt::format("[i] total time                          = {:.2f} secs\n",
                           mockturtle::to_seconds(time_total));
        out << fmt::format("[i] layout size before optimization     = {} × {}\n", x_size_before, y_size_before);
        out << fmt::format("[i] layout size after optimization      = {} × {}\n", x_size_after, y_size_after);
        out << fmt::format("[i] area reduction                      = {}%\n", area_improvement);
        out << fmt::format("[i] num. wires before optimization      = {}\n", num_wires_before);
        out << fmt::format("[i] num. wires after optimization       = {}\n", num_wires_after);
        out << fmt::format("[i] num. crossings before optimization  = {}\n", num_crossings_before);
        out << fmt::format("[i] num. crossings after optimization   = {}\n", num_crossings_after);
    }
};

namespace detail
{

/**
 * This struct stores information about the fan-in and fan-out connections of a gate in a layout.
 * These fan-in and fan-outs are the preceding and succeeding gates in the logic network.
 * It contains vectors for fan-ins, fan-outs, and temporary coordinates to clear before routing.
 * Additionally, it includes layout coordinate paths for routing signals between the gate and its fan-in/fan-out
 * connections.
 *
 * @tparam Lyt Cartesian gate-level layout type.
 */
template <typename Lyt>
struct fanin_fanout_data
{
    /**
     * Represents one fan-out target and the output pin of the moved gate that feeds this target.
     */
    struct fanout_target
    {
        /**
         * Tile position of the fan-out target.
         */
        tile<Lyt> target{};
        /**
         * Output pin index used at the moved gate to feed the fan-out target.
         */
        uint8_t source_output{0u};
    };

    /**
     * This vector holds all fan-in signals to the gate including output pin indices.
     */
    std::vector<mockturtle::signal<Lyt>> fanins;

    /**
     * This vector holds all fan-out targets and the output pin used to drive them.
     */
    std::vector<fanout_target> fanouts;

    /**
     * During the gate relocation process, this vector holds temporary layout coordinates that need to be cleared or
     * reset.
     */
    std::vector<tile<Lyt>> to_clear;

    /**
     * This layout_coordinate_path object represents the path for routing signals from the first fan-in
     * to the gate within the layout.
     */
    layout_coordinate_path<Lyt> route_fanin_1_to_gate;

    /**
     * This layout_coordinate_path object represents the path for routing signals from the second fan-in
     * to the gate within the layout.
     */
    layout_coordinate_path<Lyt> route_fanin_2_to_gate;

    /**
     * This layout_coordinate_path object represents the path for routing signals from the gate to
     * the first fan-out within the layout.
     */
    layout_coordinate_path<Lyt> route_gate_to_fanout_1;

    /**
     * This layout_coordinate_path object represents the path for routing signals from the gate to
     * the second fan-out within the layout.
     */
    layout_coordinate_path<Lyt> route_gate_to_fanout_2;
};
/**
 * Utility function that moves outputs from the last row to the previous row, and from the last column to the previous
 * column, if possible.
 *
 * @tparam Lyt Cartesian gate-level layout type.
 * @param lyt Gate-level layout.
 */
template <typename Lyt>
void optimize_output_positions(Lyt& lyt) noexcept
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_cartesian_layout_v<Lyt>, "Lyt is not a Cartesian layout");

    bool optimizable = true;

    for (uint64_t x = 0; x <= lyt.x(); ++x)
    {
        if (!(lyt.is_empty_tile({x, lyt.y()}) ||
              (lyt.is_po_tile({x, lyt.y(), 0}) && lyt.is_empty_tile({x + 1, lyt.y() - 1, 0}) && (x < lyt.x()))))
        {
            optimizable = false;
        }
    }

    if (optimizable)
    {
        for (uint64_t x = 0; x < lyt.x(); ++x)
        {
            if (lyt.is_po_tile({x, lyt.y(), 0}))
            {
                std::vector<mockturtle::signal<Lyt>> signals{};
                signals.reserve(lyt.fanin_size(lyt.get_node({x, lyt.y()})));
                lyt.foreach_fanin(lyt.get_node({x, lyt.y()}),
                                  [&signals](const auto& fanin) { signals.push_back(fanin); });
                lyt.move_node(lyt.get_node({x, lyt.y()}), {x + 1, lyt.y() - 1, 0}, signals);
            }
        }
    }

    optimizable = true;

    for (uint64_t y = 0; y <= lyt.y(); ++y)
    {
        if (!(lyt.is_empty_tile({lyt.x(), y}) ||
              (lyt.is_po_tile({lyt.x(), y, 0}) && lyt.is_empty_tile({lyt.x() - 1, y + 1, 0}) && (y < lyt.y()))))
        {
            optimizable = false;
        }
    }

    if (optimizable)
    {
        for (uint64_t y = 0; y < lyt.y(); ++y)
        {
            if (lyt.is_po_tile({lyt.x(), y, 0}))
            {
                std::vector<mockturtle::signal<Lyt>> signals{};
                signals.reserve(lyt.fanin_size(lyt.get_node({lyt.x(), y})));
                lyt.foreach_fanin(lyt.get_node({lyt.x(), y}),
                                  [&signals](const auto& fanin) { signals.push_back(fanin); });
                lyt.move_node(lyt.get_node({lyt.x(), y}), {lyt.x() - 1, y + 1, 0}, signals);
            }
        }
    }
    // calculate bounding box
    auto bounding_box = bounding_box_2d(lyt);
    lyt.resize({bounding_box.get_max().x, bounding_box.get_max().y, lyt.z()});

    // check for misplaced POs in second last row and move them one row down
    for (uint64_t x = 0; x < lyt.x(); ++x)
    {
        if (lyt.is_po_tile({x, lyt.y() - 1, 0}))
        {
            // get fanin signal of the PO
            std::vector<mockturtle::signal<Lyt>> signals{};
            signals.reserve(lyt.fanin_size(lyt.get_node({x, lyt.y()})));
            lyt.foreach_fanin(lyt.get_node({x, lyt.y() - 1}),
                              [&signals](const auto& fanin) { signals.push_back(fanin); });

            // move PO one row down
            lyt.move_node(lyt.get_node({x, lyt.y() - 1}), {x, lyt.y(), 0}, {});

            // create a wire segment at the previous location of the PO and connect it with its fanin
            lyt.create_buf(signals[0], {x, lyt.y() - 1});

            // connect the PO with the new wire segment
            lyt.move_node(lyt.get_node({x, lyt.y()}), {x, lyt.y(), 0},
                          {lyt.make_signal(lyt.get_node({x, lyt.y() - 1}))});
        }
    }

    // check for misplaced POs in second last column and move them one column to the right
    for (uint64_t y = 0; y < lyt.y(); ++y)
    {
        if (lyt.is_po_tile({lyt.x() - 1, y, 0}))
        {
            // get fanin signal of the PO
            std::vector<mockturtle::signal<Lyt>> signals{};
            signals.reserve(lyt.fanin_size(lyt.get_node({lyt.x(), y})));
            lyt.foreach_fanin(lyt.get_node({lyt.x() - 1, y}),
                              [&signals](const auto& fanin) { signals.push_back(fanin); });

            // move PO one column to the right
            lyt.move_node(lyt.get_node({lyt.x() - 1, y}), {lyt.x(), y, 0}, {});

            // create a wire segment at the previous location of the PO and connect it with its fanin
            lyt.create_buf(signals[0], {lyt.x() - 1, y});

            // connect the PO with the new wire segment
            lyt.move_node(lyt.get_node({lyt.x(), y}), {lyt.x(), y, 0},
                          {lyt.make_signal(lyt.get_node({lyt.x() - 1, y}))});
        }
    }

    // update bounding box
    bounding_box.update_bounding_box();
    lyt.resize({bounding_box.get_max().x, bounding_box.get_max().y, lyt.z()});

    // check if PO is located in bottom right corner and relocation would save more tiles (only possible for layouts
    // with a single PO)
    if (lyt.is_po_tile({lyt.x(), lyt.y(), 0}) && (lyt.num_pos() == 1))
    {
        // check if relocation would save tiles
        if (lyt.has_western_incoming_signal({lyt.x(), lyt.y(), 0}) &&
            ((lyt.x() * (lyt.y() + 2)) < ((lyt.x() + 1) * (lyt.y() + 1))))
        {
            // get fanin signal of the PO
            std::vector<mockturtle::signal<Lyt>> signals{};
            signals.reserve(lyt.fanin_size(lyt.get_node({lyt.x(), lyt.y()})));
            lyt.foreach_fanin(lyt.get_node({lyt.x(), lyt.y()}),
                              [&signals](const auto& fanin) { signals.push_back(fanin); });

            // resize layout
            lyt.resize({lyt.x(), lyt.y() + 1, lyt.z()});

            // move PO one tile down and to the left
            lyt.move_node(lyt.get_node({lyt.x(), lyt.y() - 1}), {lyt.x() - 1, lyt.y(), 0}, signals);
        }
        // check if relocation would save tiles
        else if (lyt.has_northern_incoming_signal({lyt.x(), lyt.y(), 0}) &&
                 (((lyt.x() + 2) * lyt.y()) < ((lyt.x() + 1) * (lyt.y() + 1))))
        {
            // get fanin signal of the PO
            std::vector<mockturtle::signal<Lyt>> signals{};
            signals.reserve(lyt.fanin_size(lyt.get_node({lyt.x(), lyt.y()})));
            lyt.foreach_fanin(lyt.get_node({lyt.x(), lyt.y()}),
                              [&signals](const auto& fanin) { signals.push_back(fanin); });

            // resize layout
            lyt.resize({lyt.x() + 1, lyt.y(), lyt.z()});

            // move PO one tile up and to the right
            lyt.move_node(lyt.get_node({lyt.x() - 1, lyt.y()}), {lyt.x(), lyt.y() - 1, 0}, signals);
        }
    }
}

/**
 * Utility function that checks and optimizes PO positions after each gate relocation iteration.
 * This function moves POs that are not optimally positioned (e.g., in second rightmost or second bottom positions)
 * to the optimal border positions by inserting buffer gates where the POs were.
 *
 * @tparam Lyt Cartesian gate-level layout type.
 * @param lyt Gate-level layout.
 * @param moved_gates Moved gates counter to decrement if PO is moved.
 */
template <typename Lyt>
void check_and_optimize_po_positions(Lyt& lyt, uint64_t& moved_gates) noexcept
{
    // check that all POs are at the right (x = lyt.x()) or bottom (y = lyt.y()) border
    lyt.foreach_po(
        [&lyt, &moved_gates](const auto& po) noexcept
        {
            if (const auto tile = lyt.get_tile(lyt.get_node(po));
                !(lyt.is_at_eastern_border(tile) || lyt.is_at_southern_border(tile)))
            {
                if ((tile.x == lyt.x() - 1) && (lyt.is_empty_tile({lyt.x(), tile.y})))
                {
                    // get fanin signal of the PO
                    std::vector<mockturtle::signal<Lyt>> signals{};
                    lyt.foreach_fanin(lyt.get_node(tile), [&signals](const auto& fanin) { signals.push_back(fanin); });

                    // move PO to the rightmost border
                    lyt.move_node(lyt.get_node(tile), {lyt.x(), tile.y, 0}, {});

                    // create a buffer at the previous location of the PO and connect it with its fanin
                    lyt.create_buf(signals[0], {lyt.x() - 1, tile.y});

                    // connect the PO with the new buffer
                    lyt.move_node(lyt.get_node({lyt.x(), tile.y}), {lyt.x(), tile.y, 0},
                                  {lyt.make_signal(lyt.get_node({lyt.x() - 1, tile.y}))});

                    --moved_gates;
                }
                else if ((tile.y == lyt.y() - 1) && (lyt.is_empty_tile({tile.x, lyt.y()})))
                {
                    // get fanin signal of the PO
                    std::vector<mockturtle::signal<Lyt>> signals{};
                    lyt.foreach_fanin(lyt.get_node(tile), [&signals](const auto& fanin) { signals.push_back(fanin); });

                    // move PO to the bottom border
                    lyt.move_node(lyt.get_node(tile), {tile.x, lyt.y(), 0}, {});

                    // create a buffer at the previous location of the PO and connect it with its fanin
                    lyt.create_buf(signals[0], {tile.x, lyt.y() - 1});

                    // connect the PO with the new buffer
                    lyt.move_node(lyt.get_node({tile.x, lyt.y()}), {tile.x, lyt.y(), 0},
                                  {lyt.make_signal(lyt.get_node({tile.x, lyt.y() - 1}))});

                    --moved_gates;
                }
            }
        });

    // update bounding box after PO optimizations
    const auto bounding_box = bounding_box_2d(lyt);
    lyt.resize({bounding_box.get_max().x, bounding_box.get_max().y, lyt.z()});
}
/**
 * Custom comparison function for sorting tiles based on the sum of their coordinates that breaks ties based on the
 * x-coordinate.
 *
 * @tparam Lyt Cartesian gate-level layout type.
 * @param a First tile to compare.
 * @param b Second tile to compare.
 * @return `true` iff `a < b` based on the aforementioned rule.
 */
template <typename Lyt>
bool compare_gate_tiles(const tile<Lyt>& a, const tile<Lyt>& b)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_cartesian_layout_v<Lyt>, "Lyt is not a Cartesian layout");

    return static_cast<bool>(std::make_pair(a.x + a.y, a.x) < std::make_pair(b.x + b.y, b.x));
}

template <typename Lyt>
class post_layout_optimization_impl
{
  public:
    post_layout_optimization_impl(const Lyt& lyt, const post_layout_optimization_params& p,
                                  post_layout_optimization_stats& st) :
            plyt{lyt},
            ps{p},
            pst{st},
            start{std::chrono::high_resolution_clock::now()}
    {}

    void run()
    {
        static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
        static_assert(is_cartesian_layout_v<Lyt>, "Lyt is not a Cartesian layout");

        // start the stopwatch to measure total optimization time
        const mockturtle::stopwatch stop{pst.time_total};

        // record initial layout statistics
        pst.x_size_before        = plyt.x() + 1;
        pst.y_size_before        = plyt.y() + 1;
        pst.num_wires_before     = plyt.num_wires() - plyt.num_pis() - plyt.num_pos();
        pst.num_crossings_before = plyt.num_crossings();

        // determine the maximum number of gate relocations
        max_gate_relocations = ps.max_gate_relocations.value_or((plyt.x() + 1) * (plyt.y() + 1));

        // create an obstruction layout based on the original layout
        auto layout = obstruction_layout<Lyt>(plyt);

        // initialize flags to control the optimization loop
        bool moved_at_least_one_gate = true;
        bool reduced_wiring          = true;
        timeout_limit_reached        = false;

        if (max_gate_relocations == 0)
        {
            // update the remaining timeout
            wiring_reduction_params.timeout = update_timeout();

            if (!timeout_limit_reached)
            {
                fiction::wiring_reduction(layout, wiring_reduction_params, &wiring_reduction_stats);
            }
        }
        else
        {
            // iteratively optimize the layout until no more improvements can be made or timeout is reached
            while ((moved_at_least_one_gate || reduced_wiring) && !timeout_limit_reached)
            {
                reduced_wiring = false;

                // update the remaining timeout
                wiring_reduction_params.timeout = update_timeout();

                if (moved_at_least_one_gate && !ps.optimize_pos_only && !timeout_limit_reached)
                {
                    fiction::wiring_reduction(layout, wiring_reduction_params, &wiring_reduction_stats);

                    // check if wiring reduction made any improvements
                    if (wiring_reduction_stats.area_improvement != 0ull ||
                        wiring_reduction_stats.wiring_improvement != 0ull)
                    {
                        reduced_wiring = true;
                    }
                }

                // gather all relevant gate tiles for relocation
                std::vector<tile<Lyt>> gate_tiles{};
                gate_tiles.reserve(layout.num_gates() + layout.num_pis() + layout.num_pos());
                layout.foreach_node(
                    [&layout, &gate_tiles](const auto& node) noexcept
                    {
                        if (const tile<Lyt> gate_tile = layout.get_tile(node);
                            (layout.is_gate(node) && !layout.is_wire(node)) || layout.is_fanout(node) ||
                            layout.is_pi_tile(gate_tile) || layout.is_po_tile(gate_tile))
                        {
                            layout.obstruct_coordinate({gate_tile.x, gate_tile.y, 1});
                            gate_tiles.emplace_back(gate_tile);
                        }
                    });

                // sort the gate tiles using the custom comparator
                std::sort(gate_tiles.begin(), gate_tiles.end(), compare_gate_tiles<Lyt>);

                max_non_po = tile<Lyt>{0, 0};
                // determine minimal border for POs
                for (const auto& gate_tile : gate_tiles)
                {
                    if (!layout.is_po_tile(gate_tile))
                    {
                        max_non_po.x = std::max(max_non_po.x, gate_tile.x);
                        max_non_po.y = std::max(max_non_po.y, gate_tile.y);
                    }
                }

                // reset the gate movement flag
                moved_at_least_one_gate = false;
                uint64_t moved_gates    = 0;

                // attempt to relocate each gate tile
                for (const auto& gate_tile : gate_tiles)
                {
                    if (!timeout_limit_reached)
                    {
                        if (!ps.optimize_pos_only || (ps.optimize_pos_only && layout.is_po_tile(gate_tile)))
                        {
                            if (improve_gate_location(layout, gate_tile))
                            {
                                ++moved_gates;
                            }
                        }

                        // update the remaining timeout after each relocation attempt
                        update_timeout();
                    }
                }

                // resize the layout to fit within the new bounding box after relocations
                const auto bounding_box = bounding_box_2d(layout);
                layout.resize({bounding_box.get_max().x, bounding_box.get_max().y, layout.z()});

                // check and optimize PO positions after each full gate relocation iteration
                check_and_optimize_po_positions(layout, moved_gates);

                if (moved_gates > 0)
                {
                    moved_at_least_one_gate = true;
                }
            }
        }

        // if the optimization did not time out, optimize the output positions
        if (!timeout_limit_reached)
        {
            optimize_output_positions(layout);
        }

        // final bounding box calculation and layout resizing
        const auto final_bounding_box = bounding_box_2d(layout);
        layout.resize({final_bounding_box.get_max().x, final_bounding_box.get_max().y, layout.z()});

        // update final layout statistics
        pst.x_size_after = layout.x() + 1;
        pst.y_size_after = layout.y() + 1;

        const uint64_t area_before = pst.x_size_before * pst.y_size_before;
        const uint64_t area_after  = pst.x_size_after * pst.y_size_after;

        double area_percentage_difference =
            static_cast<double>(area_before - area_after) / static_cast<double>(area_before) * 100.0;
        pst.area_improvement = std::round(area_percentage_difference * 100) / 100.0;

        pst.num_wires_after     = layout.num_wires() - layout.num_pis() - layout.num_pos();
        pst.num_crossings_after = layout.num_crossings();
    }

  private:
    /**
     * Alias for an obstruction layout based on the given layout type.
     */
    using ObstrLyt = obstruction_layout<Lyt>;
    /**
     * 2DDWave-clocked Cartesian gate-level layout to optimize.
     */
    const Lyt& plyt;
    /**
     * Post-layout optimization parameters.
     */
    post_layout_optimization_params ps;
    /**
     * Statistics about the post-layout optimization process.
     */
    post_layout_optimization_stats& pst;
    /**
     * Start time.
     */
    std::chrono::time_point<std::chrono::high_resolution_clock> start;
    /**
     * Maximum number of relocations to try for each gate.
     */
    std::optional<uint64_t> max_gate_relocations = std::nullopt;
    /**
     * Maximum coordinate of all gates that are not POs.
     */
    tile<Lyt> max_non_po{0, 0};
    /**
     * Timeout limit reached.
     */
    bool timeout_limit_reached = false;
    /**
     * Wiring reduction parameters.
     */
    fiction::wiring_reduction_params wiring_reduction_params{};
    /**
     * Wiring reduction stats.
     */
    fiction::wiring_reduction_stats wiring_reduction_stats{};
    /**
     * Utility function to move wires that cross over empty tiles down one layer. This can happen if the wiring of a
     * gate is deleted.
     *
     * @param lyt Obstructed gate-level layout.
     * @param deleted_coords Tiles that got deleted.
     */
    void fix_wires(ObstrLyt& lyt, const std::vector<tile<ObstrLyt>>& deleted_coords) noexcept
    {
        phmap::parallel_flat_hash_set<tile<ObstrLyt>> moved_tiles{};
        moved_tiles.reserve(deleted_coords.size());
        for (const auto& deleted_tile : deleted_coords)
        {
            const auto ground = lyt.below(deleted_tile);
            const auto above  = lyt.above(deleted_tile);

            if (lyt.is_empty_tile(ground) && lyt.is_wire_tile(above))
            {
                std::vector<mockturtle::signal<ObstrLyt>> incoming_signals{};
                incoming_signals.reserve(1);
                lyt.foreach_fanin(lyt.get_node(above),
                                  [&incoming_signals](const auto& fin) { incoming_signals.push_back(fin); });
                if (incoming_signals.empty())
                {
                    continue;
                }

                const auto incoming_signal = incoming_signals.front();
                const auto outgoing_tiles  = lyt.outgoing_data_flow(above);
                if (outgoing_tiles.empty())
                {
                    continue;
                }
                const auto outgoing_tile = outgoing_tiles.front();

                // move wire from z=1 to z=0
                lyt.move_node(lyt.get_node(above), ground, {incoming_signal});

                // if outgoing tile has other incoming signals (e.g. AND), update children
                std::vector<mockturtle::signal<ObstrLyt>> in_flow{};
                in_flow.reserve(lyt.fanin_size(lyt.get_node(outgoing_tile)));
                lyt.foreach_fanin(lyt.get_node(outgoing_tile), [&in_flow](const auto& fin) { in_flow.push_back(fin); });

                if (!in_flow.empty())
                {
                    const auto front      = in_flow.front();
                    const auto front_tile = static_cast<tile<ObstrLyt>>(front);

                    if (std::find(deleted_coords.cbegin(), deleted_coords.cend(), front_tile) ==
                            deleted_coords.cend() ||
                        std::find(moved_tiles.cbegin(), moved_tiles.cend(), front_tile) != moved_tiles.cend())
                    {
                        lyt.move_node(lyt.get_node(outgoing_tile), outgoing_tile,
                                      {lyt.make_signal(lyt.get_node(ground)), front});
                    }
                }
                // otherwise, the wire is the only incoming signal
                else
                {
                    lyt.move_node(lyt.get_node(outgoing_tile), outgoing_tile, {lyt.make_signal(lyt.get_node(ground))});
                }

                if constexpr (has_is_obstructed_coordinate_v<ObstrLyt>)
                {
                    // update obstructions
                    lyt.obstruct_coordinate(ground);
                    lyt.clear_obstructed_coordinate(above);
                }

                moved_tiles.insert(deleted_tile);
            }
        }
    }
    /**
     * This helper function is used to add a fanin coordinate to the appropriate route
     * based on whether it belongs to the the route from the first or second fanin to the gate.
     *
     * @param fanin The fanin coordinate to be added to the route.
     * @param is_first_fanin A boolean indicating whether this is part of the route from the first fanin to the gate.
     * @param ffd Reference to the fanin_fanout_data structure containing the routes.
     */
    void add_fanin_to_route(const tile<ObstrLyt>& fanin, bool is_first_fanin, fanin_fanout_data<ObstrLyt>& ffd) noexcept
    {
        auto& target_route = is_first_fanin ? ffd.route_fanin_1_to_gate : ffd.route_fanin_2_to_gate;

        target_route.insert(target_route.cbegin(), fanin);
    }
    /**
     * This helper function is used to add a fanout coordinate to the appropriate route
     * based on whether it belongs to the the route from the gate to the first or second fanout.
     *
     * @param fanout The fanout coordinate to be added to the route.
     * @param is_first_fanout A boolean indicating whether it belongs to the route from the gate to the first fanout.
     * @param ffd Reference to the fanin_fanout_data structure containing the routes.
     */
    void add_fanout_to_route(const tile<ObstrLyt>& fanout, bool is_first_fanout,
                             fanin_fanout_data<ObstrLyt>& ffd) noexcept
    {
        auto& target_route = is_first_fanout ? ffd.route_gate_to_fanout_1 : ffd.route_gate_to_fanout_2;

        target_route.push_back(fanout);
    }
    /**
     * Utility function to trace back fanins and fanouts of a gate. Based on the gate to be moved, this function returns
     * the location of the fanins and fanouts, as well as the wiring in between them. Additionally, all wire tiles
     * between fanins and the gate, as well as between the gate and fanouts are collected for deletion.
     *
     * @param lyt Obstructed gate-level layout.
     * @param op coordinate of the gate to be moved.
     * @return fanin and fanout gates, wires to be deleted and old routing paths.
     */
    [[nodiscard]] fanin_fanout_data<ObstrLyt> get_fanin_and_fanouts(const ObstrLyt&       lyt,
                                                                    const tile<ObstrLyt>& op) noexcept
    {
        fanin_fanout_data<ObstrLyt> ffd{};

        mockturtle::signal<ObstrLyt> fanin1{};
        mockturtle::signal<ObstrLyt> fanin2{};
        bool                         has_fanin1 = false;
        bool                         has_fanin2 = false;

        typename fanin_fanout_data<ObstrLyt>::fanout_target fanout1{};
        typename fanin_fanout_data<ObstrLyt>::fanout_target fanout2{};
        bool                                                has_fanout1 = false;
        bool                                                has_fanout2 = false;

        phmap::parallel_flat_hash_set<tile<ObstrLyt>> fanins_set{};
        fanins_set.reserve(lyt.num_wires() + lyt.num_gates() - 2);
        phmap::parallel_flat_hash_set<tile<ObstrLyt>> fanouts_set{};
        fanouts_set.reserve(lyt.num_wires() + lyt.num_gates() - 2);

        lyt.foreach_fanin(
            lyt.get_node(op),
            [&lyt, &fanins_set, &op, &fanin1, &fanin2, &has_fanin1, &has_fanin2, &ffd, this](const auto& fin)
            {
                auto fanin         = static_cast<tile<ObstrLyt>>(fin);
                auto source_signal = fin;

                if (fanins_set.find(fanin) == fanins_set.cend())
                {
                    // add fanin to the respective route
                    add_fanin_to_route(op, fanins_set.empty(), ffd);
                    add_fanin_to_route(fanin, fanins_set.empty(), ffd);

                    // continue until gate or primary input (PI) is found
                    while (lyt.is_wire_tile(fanin) && lyt.fanout_size(lyt.get_node(fanin)) == 1 &&
                           !lyt.is_pi_tile(fanin))
                    {
                        ffd.to_clear.push_back(fanin);
                        std::vector<mockturtle::signal<ObstrLyt>> incoming_signals{};
                        incoming_signals.reserve(1);
                        lyt.foreach_fanin(lyt.get_node(fanin),
                                          [&incoming_signals](const auto& in) { incoming_signals.push_back(in); });
                        if (incoming_signals.empty())
                        {
                            break;
                        }
                        source_signal = incoming_signals.front();
                        fanin         = static_cast<tile<ObstrLyt>>(source_signal);

                        // add fanin to the respective route
                        add_fanin_to_route(fanin, fanins_set.empty(), ffd);
                    }

                    source_signal.index = static_cast<uint64_t>(fanin);

                    // set the respective fanin based on the route
                    if (fanins_set.empty())
                    {
                        fanin1     = source_signal;
                        has_fanin1 = true;
                    }
                    else
                    {
                        fanin2     = source_signal;
                        has_fanin2 = true;
                    }

                    fanins_set.insert(fanin);
                }
            });
        // same for fanouts
        lyt.foreach_fanout(
            lyt.get_node(op),
            [&lyt, &fanouts_set, &op, &fanout1, &fanout2, &has_fanout1, &has_fanout2, &ffd, this](const auto& fout)
            {
                tile<ObstrLyt> fanout = lyt.get_tile(fout);
                uint8_t        source_output{0u};

                lyt.foreach_fanin(lyt.get_node(fanout),
                                  [&source_output, &op](const auto& fin) -> bool
                                  {
                                      if (static_cast<tile<ObstrLyt>>(fin) == op)
                                      {
                                          source_output = fin.output;
                                          return false;
                                      }
                                      return true;
                                  });

                if (fanouts_set.find(fanout) == fanouts_set.cend())
                {

                    // add fanout to the respective route
                    add_fanout_to_route(op, fanouts_set.empty(), ffd);
                    add_fanout_to_route(fanout, fanouts_set.empty(), ffd);

                    // continue until gate or primary output (PO) is found
                    while (lyt.is_wire_tile(fanout) && lyt.fanout_size(lyt.get_node(fanout)) != 0 &&
                           lyt.fanout_size(lyt.get_node(fanout)) != 2)
                    {
                        ffd.to_clear.push_back(fanout);
                        fanout = lyt.outgoing_data_flow(fanout).front();

                        // add fanout to the respective route
                        add_fanout_to_route(fanout, fanouts_set.empty(), ffd);
                    }

                    // set the respective fanout based on the route
                    if (fanouts_set.empty())
                    {
                        fanout1     = {fanout, source_output};
                        has_fanout1 = true;
                    }
                    else
                    {
                        fanout2     = {fanout, source_output};
                        has_fanout2 = true;
                    }

                    fanouts_set.insert(fanout);
                }
            });

        // add fanins and fanouts if existing
        if (has_fanin1)
        {
            ffd.fanins.push_back(fanin1);
        }
        if (has_fanin2)
        {
            ffd.fanins.push_back(fanin2);
        }
        if (has_fanout1)
        {
            ffd.fanouts.push_back(fanout1);
        }
        if (has_fanout2)
        {
            ffd.fanouts.push_back(fanout2);
        }

        return ffd;
    }
    /**
     * Distinguishes direct launch directions for two-output gates in Cartesian 2DDWave layouts.
     */
    enum class launch_side : std::uint8_t
    {
        NONE,
        EAST,
        SOUTH,
        OTHER
    };
    /**
     * Summarizes immediate launch usage of a placed multi-output gate.
     */
    struct multioutput_launch_usage
    {
        std::array<launch_side, 2u> pin_sides{{launch_side::NONE, launch_side::NONE}};
        uint32_t                    east_fanouts{0u};
        uint32_t                    south_fanouts{0u};
        uint32_t                    other_fanouts{0u};
        bool                        inconsistent_pin_launch{false};
    };
    /**
     * Determines on which outgoing side a route leaves a source tile.
     *
     * @param lyt Layout containing the route.
     * @param source Source tile.
     * @param successor First successor tile of the route.
     * @return Launch side of the step from `source` to `successor`.
     */
    [[nodiscard]] static launch_side determine_launch_side(const ObstrLyt& lyt, const tile<ObstrLyt>& source,
                                                           const tile<ObstrLyt>& successor) noexcept
    {
        const auto source_projected    = lyt.below(source);
        const auto successor_projected = lyt.below(successor);

        if (successor_projected == lyt.east(source_projected))
        {
            return launch_side::EAST;
        }
        if (successor_projected == lyt.south(source_projected))
        {
            return launch_side::SOUTH;
        }

        return launch_side::OTHER;
    }
    /**
     * Collects immediate launch usage for a placed multi-output gate.
     *
     * @param lyt Layout containing the gate.
     * @param source Tile of the placed gate.
     * @return Summary of currently used launch sides and output pins.
     */
    [[nodiscard]] multioutput_launch_usage collect_multioutput_launch_usage(const ObstrLyt&       lyt,
                                                                            const tile<ObstrLyt>& source) const noexcept
    {
        multioutput_launch_usage usage{};
        const auto               source_node = lyt.get_node(source);

        if (!lyt.is_multioutput(source_node))
        {
            return usage;
        }

        const auto source_projected = lyt.below(source);

        lyt.foreach_fanout(source_node,
                           [&lyt, &source, &source_projected, &usage, this](const auto& fout)
                           {
                               const auto fanout_tile      = lyt.get_tile(fout);
                               const auto launch           = determine_launch_side(lyt, source, fanout_tile);
                               bool       has_output_index = false;
                               uint8_t    output_index{0u};

                               lyt.foreach_fanin(
                                   fout,
                                   [&lyt, &source_projected, &has_output_index, &output_index](const auto& fin)
                                   {
                                       if (lyt.below(static_cast<tile<ObstrLyt>>(fin)) == source_projected)
                                       {
                                           has_output_index = true;
                                           output_index     = fin.output;
                                       }
                                   });

                               if (!has_output_index || output_index >= usage.pin_sides.size())
                               {
                                   usage.inconsistent_pin_launch = true;
                                   return;
                               }

                               if (usage.pin_sides[output_index] == launch_side::NONE)
                               {
                                   usage.pin_sides[output_index] = launch;
                               }
                               else if (usage.pin_sides[output_index] != launch)
                               {
                                   usage.inconsistent_pin_launch = true;
                               }

                               switch (launch)
                               {
                                   case launch_side::EAST: ++usage.east_fanouts; break;
                                   case launch_side::SOUTH: ++usage.south_fanouts; break;
                                   case launch_side::OTHER: ++usage.other_fanouts; break;
                                   case launch_side::NONE: break;
                               }
                           });

        return usage;
    }
    /**
     * Checks whether a candidate path uses a legal immediate launch side for a two-output source gate.
     *
     * @param lyt Layout in which the path is considered.
     * @param source_signal Source signal launched by the path.
     * @param source Source tile of the path.
     * @param path Candidate route from `source`.
     * @return `true` iff the first routed step is legal.
     */
    [[nodiscard]] bool uses_legal_multioutput_launch_side(const ObstrLyt&                         lyt,
                                                          const mockturtle::signal<ObstrLyt>&     source_signal,
                                                          const tile<ObstrLyt>&                   source,
                                                          const layout_coordinate_path<ObstrLyt>& path) const noexcept
    {
        if (path.size() < 2)
        {
            return false;
        }

        if (lyt.is_empty_tile(source))
        {
            return true;
        }

        const auto source_node = lyt.get_node(source);

        if (!lyt.is_multioutput(source_node) || source_signal.output >= 2u)
        {
            return true;
        }

        const auto launch = determine_launch_side(lyt, source, path[1]);

        if ((launch != launch_side::EAST) && (launch != launch_side::SOUTH))
        {
            return false;
        }

        const auto usage = collect_multioutput_launch_usage(lyt, source);

        if (usage.inconsistent_pin_launch || usage.other_fanouts != 0u || usage.east_fanouts > 1u ||
            usage.south_fanouts > 1u)
        {
            return false;
        }

        if (usage.pin_sides[source_signal.output] != launch_side::NONE)
        {
            return usage.pin_sides[source_signal.output] == launch;
        }

        const auto other_output = static_cast<uint8_t>(source_signal.output == 0u ? 1u : 0u);

        if (usage.pin_sides[other_output] == launch_side::EAST)
        {
            return launch == launch_side::SOUTH;
        }
        if (usage.pin_sides[other_output] == launch_side::SOUTH)
        {
            return launch == launch_side::EAST;
        }

        return true;
    }
    /**
     * This helper function computes a path between two coordinates using the A* algorithm.
     * It then obstructs the tiles along the path in the given layout.
     *
     * @param lyt Obstructed gate-level layout.
     * @param source_signal Optional source signal used to enforce output-pin-aware launch sides.
     * @param start_tile The starting coordinate of the path.
     * @param end_tile The ending coordinate of the path.
     * @return The computed path as a sequence of coordinates in the layout.
     */
    layout_coordinate_path<ObstrLyt>
    get_path_and_obstruct(ObstrLyt& lyt, const tile<ObstrLyt>& start_tile, const tile<ObstrLyt>& end_tile,
                          const std::optional<mockturtle::signal<ObstrLyt>>& source_signal = std::nullopt)
    {
        using dist = twoddwave_distance_functor<ObstrLyt, uint64_t>;
        using cost = unit_cost_functor<ObstrLyt, uint8_t>;
        static a_star_params astar_params{};
        astar_params.crossings = !ps.planar_optimization;

        const auto path =
            a_star<layout_coordinate_path<ObstrLyt>>(lyt, {start_tile, end_tile}, dist(), cost(), astar_params);

        if (source_signal.has_value() && !uses_legal_multioutput_launch_side(lyt, *source_signal, start_tile, path))
        {
            return {};
        }

        // obstruct the tiles along the computed path.
        for (const auto& tile : path)
        {
            lyt.obstruct_coordinate(tile);
        }

        return path;
    }
    /**
     * Calculates the elapsed milliseconds since the `start` time, sets the `timeout_limit_reached` flag
     * if the timeout is exceeded, and returns the remaining time.
     *
     * @return Remaining time in milliseconds before timeout, or `0` if timeout has been reached.
     */
    uint64_t update_timeout() noexcept
    {
        const auto current_time = std::chrono::high_resolution_clock::now();
        const auto elapsed_ms =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start).count());
        timeout_limit_reached = (elapsed_ms >= ps.timeout);
        return timeout_limit_reached ? 0 : ps.timeout - elapsed_ms;
    }
    /**
     * Attempts to relocate a gate to a new position within the layout and updates routing connections accordingly.
     *
     * @param lyt                  Obstructed gate-level layout being optimized.
     * @param new_pos              The target tile position to which the gate is to be relocated.
     * @param num_gate_relocations Reference to a counter tracking the number of gate relocations performed.
     * @param current_pos          Reference to the current position of the gate being relocated. This will be updated
     * upon successful relocation.
     * @param fanins               Vector containing the tile positions of all fan-in connections to the gate.
     * @param fanouts              Vector containing the tile positions of all fan-out connections from the gate.
     * @param moved_gate           Reference to a boolean flag that will be set to `true` if the gate is successfully
     * moved.
     * @param old_pos              The original tile position of the gate before the relocation attempt.
     *
     * @return `true` if the gate was successfully relocated to `new_pos` and all routing paths were established.
     *         `false` if the relocation resulted in no movement (i.e., `new_pos` is the same as `old_pos`).
     */
    bool check_new_position(ObstrLyt& lyt, const tile<ObstrLyt>& new_pos, uint64_t& num_gate_relocations,
                            tile<ObstrLyt>& current_pos, const std::vector<mockturtle::signal<ObstrLyt>>& fanins,
                            const std::vector<typename fanin_fanout_data<ObstrLyt>::fanout_target>& fanouts,
                            bool& moved_gate, const tile<ObstrLyt>& old_pos) noexcept
    {
        if (lyt.is_empty_tile(new_pos) && lyt.is_empty_tile({new_pos.x, new_pos.y, 1}))
        {
            num_gate_relocations++;
            // move gate to new positions and update obstructions
            lyt.move_node(lyt.get_node(current_pos), new_pos, {});
            lyt.obstruct_coordinate(new_pos);
            lyt.obstruct_coordinate({new_pos.x, new_pos.y, 1});
            lyt.clear_obstructed_coordinate(current_pos);
            lyt.clear_obstructed_coordinate({current_pos.x, current_pos.y, 1});

            // get paths for fanins and fanouts
            layout_coordinate_path<ObstrLyt> new_path_from_fanin_1_to_gate, new_path_from_fanin_2_to_gate,
                new_path_from_gate_to_fanout_1, new_path_from_gate_to_fanout_2;
            // get paths for fanins and fanouts
            if (!fanins.empty())
            {
                new_path_from_fanin_1_to_gate =
                    get_path_and_obstruct(lyt, static_cast<tile<ObstrLyt>>(fanins[0]), new_pos, fanins[0]);
            }

            if (fanins.size() == 2)
            {
                new_path_from_fanin_2_to_gate =
                    get_path_and_obstruct(lyt, static_cast<tile<ObstrLyt>>(fanins[1]), new_pos, fanins[1]);
            }

            if (!fanouts.empty())
            {
                new_path_from_gate_to_fanout_1 = get_path_and_obstruct(
                    lyt, new_pos, fanouts[0].target, lyt.make_signal(lyt.get_node(new_pos), fanouts[0].source_output));
            }

            if (fanouts.size() == 2)
            {
                new_path_from_gate_to_fanout_2 = get_path_and_obstruct(
                    lyt, new_pos, fanouts[1].target, lyt.make_signal(lyt.get_node(new_pos), fanouts[1].source_output));
            }

            if (!(!fanins.empty() && new_path_from_fanin_1_to_gate.empty()) &&
                !(fanins.size() == 2 && new_path_from_fanin_2_to_gate.empty()) &&
                !(!fanouts.empty() && new_path_from_gate_to_fanout_1.empty()) &&
                !(fanouts.size() == 2 && new_path_from_gate_to_fanout_2.empty()))
            {
                for (const auto& path : {new_path_from_fanin_1_to_gate, new_path_from_fanin_2_to_gate,
                                         new_path_from_gate_to_fanout_1, new_path_from_gate_to_fanout_2})
                {
                    for (const auto& tile : path)
                    {
                        lyt.obstruct_coordinate(tile);
                    }
                }

                if (!new_path_from_fanin_1_to_gate.empty())
                {
                    route_path(lyt, fanins[0], new_path_from_fanin_1_to_gate);
                }
                if (!new_path_from_fanin_2_to_gate.empty() && fanins.size() == 2)
                {
                    route_path(lyt, fanins[1], new_path_from_fanin_2_to_gate);
                }
                if (!new_path_from_gate_to_fanout_1.empty())
                {
                    route_path(lyt, lyt.make_signal(lyt.get_node(new_pos), fanouts[0].source_output),
                               new_path_from_gate_to_fanout_1);
                }
                if (!new_path_from_gate_to_fanout_2.empty() && fanouts.size() == 2)
                {
                    route_path(lyt, lyt.make_signal(lyt.get_node(new_pos), fanouts[1].source_output),
                               new_path_from_gate_to_fanout_2);
                }

                moved_gate = true;

                // update children of moved gate to preserve exact source output pins.
                std::vector<mockturtle::signal<ObstrLyt>> moved_gate_signals{};
                moved_gate_signals.reserve(lyt.fanin_size(lyt.get_node(new_pos)));
                lyt.foreach_fanin(lyt.get_node(new_pos),
                                  [&moved_gate_signals](const auto& fin) { moved_gate_signals.push_back(fin); });
                lyt.move_node(lyt.get_node(new_pos), new_pos, moved_gate_signals);

                // update children of fanouts
                for (const auto& fanout : fanouts)
                {
                    std::vector<mockturtle::signal<ObstrLyt>> signals{};
                    signals.reserve(lyt.fanin_size(lyt.get_node(fanout.target)));

                    lyt.foreach_fanin(lyt.get_node(fanout.target), [&signals](const auto& i) { signals.push_back(i); });

                    lyt.move_node(lyt.get_node(fanout.target), fanout.target, signals);
                }

                if (new_pos == old_pos)
                {
                    return false;
                }
            }
            // if no routing was found, remove added obstructions
            else
            {
                for (const auto& path : {new_path_from_fanin_1_to_gate, new_path_from_fanin_2_to_gate,
                                         new_path_from_gate_to_fanout_1, new_path_from_gate_to_fanout_2})
                {
                    for (const auto& tile : path)
                    {
                        lyt.clear_obstructed_coordinate(tile);
                    }
                }
            }

            current_pos = new_pos;
        }
        return true;
    }
    /**
     * Restores the original wiring if relocation of a gate fails.
     *
     * This function moves the gate back to its original position and reinstates the previous wiring paths
     * between the gate and its fan-in/fan-out connections. It also updates the obstructions in the layout
     * accordingly.
     *
     * @param lyt Obstructed gate-level layout.
     * @param old_path_from_fanin_1_to_gate The original routing path from the first fan-in to the gate (if exists).
     * @param old_path_from_fanin_2_to_gate The original routing path from the second fan-in to the gate (if exists).
     * @param old_path_from_gate_to_fanout_1 The original routing path from the gate to the first fan-out (if exists).
     * @param old_path_from_gate_to_fanout_2 The original routing path from the gate to the second fan-out (if exists).
     * @param current_pos Current position of the gate after relocation attempt.
     * @param old_pos Original position of the gate before relocation attempt.
     * @param fanins Vector of fanin signals connected to the gate.
     * @param fanouts Vector of fanout targets connected to the gate.
     */
    void
    restore_original_wiring(ObstrLyt& lyt, const layout_coordinate_path<ObstrLyt> old_path_from_fanin_1_to_gate,
                            const layout_coordinate_path<ObstrLyt> old_path_from_fanin_2_to_gate,
                            const layout_coordinate_path<ObstrLyt> old_path_from_gate_to_fanout_1,
                            const layout_coordinate_path<ObstrLyt> old_path_from_gate_to_fanout_2,
                            const tile<ObstrLyt>& current_pos, const tile<ObstrLyt>& old_pos,
                            const std::vector<mockturtle::signal<ObstrLyt>>&                        fanins,
                            const std::vector<typename fanin_fanout_data<ObstrLyt>::fanout_target>& fanouts) noexcept
    {
        lyt.move_node(lyt.get_node(current_pos), old_pos, {});

        if (!old_path_from_fanin_1_to_gate.empty() && !fanins.empty())
        {
            route_path(lyt, fanins[0], old_path_from_fanin_1_to_gate);
        }
        if (!old_path_from_fanin_2_to_gate.empty() && fanins.size() == 2)
        {
            route_path(lyt, fanins[1], old_path_from_fanin_2_to_gate);
        }
        if (!old_path_from_gate_to_fanout_1.empty() && !fanouts.empty())
        {
            route_path(lyt, lyt.make_signal(lyt.get_node(old_pos), fanouts[0].source_output),
                       old_path_from_gate_to_fanout_1);
        }
        if (!old_path_from_gate_to_fanout_2.empty() && fanouts.size() == 2)
        {
            route_path(lyt, lyt.make_signal(lyt.get_node(old_pos), fanouts[1].source_output),
                       old_path_from_gate_to_fanout_2);
        }

        for (const auto& r : {old_path_from_fanin_1_to_gate, old_path_from_fanin_2_to_gate,
                              old_path_from_gate_to_fanout_1, old_path_from_gate_to_fanout_2})
        {
            for (const auto& t : r)
            {
                lyt.obstruct_coordinate(t);
            }
        }

        // update obstructions
        lyt.clear_obstructed_coordinate(current_pos);
        lyt.clear_obstructed_coordinate({current_pos.x, current_pos.y, 1});
        lyt.obstruct_coordinate(old_pos);
        lyt.obstruct_coordinate({old_pos.x, old_pos.y, 1});

        // update children on old position
        std::vector<mockturtle::signal<ObstrLyt>> signals{};
        signals.reserve(lyt.fanin_size(lyt.get_node(old_pos)));

        lyt.foreach_fanin(lyt.get_node(old_pos), [&signals](const auto& i) { signals.push_back(i); });

        lyt.move_node(lyt.get_node(old_pos), old_pos, signals);

        // update children of fanouts
        for (const auto& fanout : fanouts)
        {
            std::vector<mockturtle::signal<ObstrLyt>> fout_signals{};
            fout_signals.reserve(lyt.fanin_size(lyt.get_node(fanout.target)));

            lyt.foreach_fanin(lyt.get_node(fanout.target),
                              [&fout_signals](const auto& i) { fout_signals.push_back(i); });

            lyt.move_node(lyt.get_node(fanout.target), fanout.target, fout_signals);
        }
    }
    /**
     * Utility function that moves gates to new coordinates and checks if routing is possible.
     * This includes:
     *
     * - removing the old wiring between fanins, the gate and fanouts
     * - updating the incoming signals
     * - determining coordinates that would improve the layout
     * - testing all those coordinates by moving the gate to each one and checking if a new wiring can be found
     * - if a new coordinate is found and wiring is possible, it is applied and incoming signals are updated
     * - if no better coordinate is found, the old wiring is restored
     *
     * @param lyt Obstructed gate-level layout.
     * @param old_pos Old position of the gate to be moved.
     * @return `true` if the gate was moved successfully, `false` otherwise.
     */
    bool improve_gate_location(ObstrLyt& lyt, const tile<ObstrLyt>& old_pos) noexcept
    {
        const auto& [fanins, fanouts, to_clear, old_path_from_fanin_1_to_gate, old_path_from_fanin_2_to_gate,
                     old_path_from_gate_to_fanout_1, old_path_from_gate_to_fanout_2] =
            get_fanin_and_fanouts(lyt, old_pos);

        uint64_t min_x = 0;
        uint64_t min_y = 0;

        // determine minimum coordinates for new placements
        if (!fanins.empty())
        {
            for (const auto& fanin : fanins)
            {
                const auto fanin_tile = static_cast<tile<ObstrLyt>>(fanin);
                min_x                 = std::max(min_x, fanin_tile.x);
                min_y                 = std::max(min_y, fanin_tile.y);
            }
        }

        const auto max_x        = old_pos.x;
        const auto max_y        = old_pos.y;
        const auto max_diagonal = max_x + max_y;

        auto new_pos = tile<ObstrLyt>{};

        // if gate is directly connected to one of its fanins, no improvement is possible
        for (const auto& fanin : fanins)
        {
            bool has_direct_connection = false;
            lyt.foreach_fanin(lyt.get_node(old_pos),
                              [&fanin, &has_direct_connection](const auto& i) -> bool
                              {
                                  if (i == fanin)
                                  {
                                      has_direct_connection = true;
                                      return false;
                                  }
                                  return true;
                              });
            if (has_direct_connection)
            {
                return false;
            }
        }

        // remove wiring
        for (const auto& tile : to_clear)
        {
            lyt.clear_tile(tile);
            lyt.clear_obstructed_coordinate(tile);
        }

        // remove children of gate to be moved
        lyt.resize({lyt.x() + 2, lyt.y(), lyt.z()});
        lyt.move_node(lyt.get_node(old_pos), {lyt.x(), 0}, {});

        // update children of fanouts
        for (const auto& fanout : fanouts)
        {
            std::vector<mockturtle::signal<ObstrLyt>> fins{};
            fins.reserve(2);
            lyt.foreach_fanin(lyt.get_node(fanout.target),
                              [&fins, &old_pos](const auto& i)
                              {
                                  auto fout = static_cast<tile<ObstrLyt>>(i);
                                  if (fout != old_pos)
                                  {
                                      fins.push_back(i);
                                  }
                              });

            lyt.move_node(lyt.get_node(fanout.target), fanout.target, fins);
        }

        // remove children of gate to be moved
        lyt.move_node(lyt.get_node({lyt.x(), 0}), old_pos, {});
        lyt.resize({lyt.x() - 2, lyt.y(), lyt.z()});

        // fix wires that cross over empty tiles
        fix_wires(lyt, to_clear);

        auto moved_gate  = false;
        auto current_pos = old_pos;

        uint64_t num_gate_relocations = 0;

        // iterate over layout diagonally
        for (uint64_t k = 0; k < lyt.x() + lyt.y() + 1; ++k)
        {
            for (uint64_t x = 0; x < k + 1; ++x)
            {
                const uint64_t y = k - x;

                if (moved_gate || ((num_gate_relocations >= max_gate_relocations) && !lyt.is_po_tile(current_pos)) ||
                    timeout_limit_reached)
                {
                    break;
                }

                update_timeout();
                // only check better positions
                if (lyt.y() >= y && y >= min_y && lyt.x() >= x && x >= min_x && ((x + y) <= max_diagonal) &&
                    (((x + y) < max_diagonal) || (y <= max_y)) &&
                    ((!lyt.is_pi_tile(current_pos)) || (lyt.is_pi_tile(current_pos) && (x == 0 || y == 0))) &&
                    !(lyt.is_po_tile(current_pos) && (((x < max_non_po.x) && (y < max_non_po.y)) ||
                                                      ((x + y) == static_cast<uint64_t>(old_pos.x + old_pos.y)))))
                {
                    new_pos = tile<ObstrLyt>{x, y};
                    if (!check_new_position(lyt, new_pos, num_gate_relocations, current_pos, fanins, fanouts,
                                            moved_gate, old_pos))
                    {
                        return false;
                    }
                }
            }

            if (moved_gate || ((num_gate_relocations >= max_gate_relocations) && !lyt.is_po_tile(current_pos)))
            {
                break;
            }
        }

        // if no better coordinate was found, restore old wiring
        if (!moved_gate)
        {
            restore_original_wiring(lyt, old_path_from_fanin_1_to_gate, old_path_from_fanin_2_to_gate,
                                    old_path_from_gate_to_fanout_1, old_path_from_gate_to_fanout_2, current_pos,
                                    old_pos, fanins, fanouts);
            return false;
        }

        return true;
    }
};

}  // namespace detail

/**
 * A post-layout optimization algorithm as originally proposed in \"Post-Layout Optimization for Field-coupled
 * Nanotechnologies\" by S. Hofmann, M. Walter, and R. Wille in NANOARCH 2023
 * (https://dl.acm.org/doi/10.1145/3611315.3633247) and extended in \"Efficient and Scalable Post-Layout Optimization
 * for Field-coupled Nanotechnologies\" by S. Hofmann, M. Walter, and R. Wille in TCAD 2025
 * (https://ieeexplore.ieee.org/document/10916761). It can be used to reduce the area of a given sub-optimal Cartesian
 * gate-level layout created by heuristics or machine learning. This optimization utilizes the distinct characteristics
 * of the 2DDWave clocking scheme, which only allows information flow from top to bottom and left to right, therefore
 * only aforementioned clocking scheme is supported.
 *
 * To reduce the layout area, first, gates are moved up and to the left as far as possible, including rerouting. This
 * creates more compact layouts by freeing up space to the right and bottom, as all gates were moved to the top left
 * corner.
 *
 * After moving all gates, this algorithm also checks if excess wiring exists on the layout using the `wiring_reduction`
 * algorithm (cf. `wiring_reduction.hpp`)
 *
 * As outputs have to lay on the border of a layout for better accessibility, they are also moved to new borders
 * determined based on the location of all other gates.
 *
 * @note This function requires the gate-level layout to be 2DDWave-clocked!
 *
 * @tparam Lyt Cartesian gate-level layout type.
 * @param lyt 2DDWave-clocked Cartesian gate-level layout to optimize.
 * @param ps Parameters.
 * @param pst Statistics.
 */
template <typename Lyt>
void post_layout_optimization(const Lyt& lyt, post_layout_optimization_params ps = {},
                              post_layout_optimization_stats* pst = nullptr) noexcept
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_cartesian_layout_v<Lyt>, "Lyt is not a Cartesian layout");

    // check if the clocking scheme is 2DDWave
    if (!lyt.is_clocking_scheme(clock_name::TWODDWAVE))
    {
        std::cout << "[e] the given layout has to be 2DDWave-clocked\n";
        return;
    }

    // check that all PIs are at the left (x = 0) or top (y = 0) border
    lyt.foreach_pi(
        [&lyt](const auto& pi) noexcept
        {
            if (const auto tile = lyt.get_tile(pi);
                !(lyt.is_at_northern_border(tile) || lyt.is_at_western_border(tile)))
            {
                std::cout << "[e] Invalid layout: All PIs must be located at the left (x = 0) or top (y = 0) border\n";
                return;
            }
        });

    // check all POs are at the right (x = lyt.x()) or bottom (y = lyt.y()) border
    lyt.foreach_po(
        [&lyt](const auto& po) noexcept
        {
            if (const auto tile = lyt.get_tile(lyt.get_node(po));
                !(lyt.is_at_eastern_border(tile) || lyt.is_at_southern_border(tile)))
            {
                std::cout << fmt::format(
                    "[e] Invalid layout: All POs must be located at the right (x = {}) or bottom (y = {}) border\n",
                    lyt.x(), lyt.y());
                return;
            }
        });

    // initialize stats for runtime measurement
    post_layout_optimization_stats             st{};
    detail::post_layout_optimization_impl<Lyt> p{lyt, ps, st};

    p.run();

    if (pst != nullptr)
    {
        *pst = st;
    }
}

}  // namespace fiction

#pragma GCC diagnostic pop

#endif  // FICTION_POST_LAYOUT_OPTIMIZATION_HPP
