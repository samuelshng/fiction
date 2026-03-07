/**
 * @file orthogonal_hex.hpp
 * @brief Native hexagonal orthogonal physical design with strict top-to-bottom data flow.
 */

#ifndef FICTION_ORTHOGONAL_HEX_HPP
#define FICTION_ORTHOGONAL_HEX_HPP

#include "fiction/algorithms/network_transformation/fanout_substitution.hpp"
#include "fiction/algorithms/physical_design/orthogonal.hpp"
#include "fiction/layouts/clocking_scheme.hpp"
#include "fiction/networks/netlist.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/name_utils.hpp"
#include "fiction/utils/network_utils.hpp"
#include "fiction/utils/placement_utils.hpp"
#include "fiction/utils/routing_utils.hpp"

#include <mockturtle/traits.hpp>
#include <mockturtle/utils/node_map.hpp>
#include <mockturtle/utils/stopwatch.hpp>
#include <mockturtle/views/fanout_view.hpp>
#include <mockturtle/views/topo_view.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <sstream>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fiction
{

namespace detail
{

/**
 * @brief Hexagonal implementation of orthogonal physical design.
 *
 * Unlike the cartesian orthogonal flow, this implementation is deliberately top-down:
 * all PIs are placed on the northern border, all POs on the southern border, and every
 * routed edge uses only downward hexagonal directions.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @tparam Ntk Specification network type.
 */
template <typename Lyt, typename Ntk>
class orthogonal_hex_impl
{
  private:
    /**
     * @brief Internal technology-mapped and fanout-normalized network type.
     */
    using network_type = mockturtle::names_view<netlist>;
    /**
     * @brief Fanout-annotated view of the working network.
     */
    using fanout_network_type = mockturtle::fanout_view<network_type>;
    /**
     * @brief Topological traversal view of the working network.
     */
    using topo_network_type = mockturtle::topo_view<fanout_network_type>;
    /**
     * @brief Coordinate type of the target layout.
     */
    using tile_type = tile<Lyt>;
    /**
     * @brief Signal type of the target layout.
     */
    using signal_type = mockturtle::signal<Lyt>;
    /**
     * @brief Coordinate path type used during monotone routing.
     */
    using path_type = layout_coordinate_path<Lyt>;

    /**
     * @brief Tiny trait that detects whether a signal type carries an output index.
     *
     * @tparam Signal Signal type to inspect.
     * @tparam Dummy Helper parameter.
     */
    template <typename Signal, typename Dummy = void>
    struct has_output_field : std::false_type
    {};
    /**
     * @brief Specialization for signal types with an `output` field.
     *
     * @tparam Signal Signal type to inspect.
     */
    template <typename Signal>
    struct has_output_field<Signal, std::void_t<decltype(std::declval<Signal&>().output)>> : std::true_type
    {};

    /**
     * @brief Planned route from an existing source tile to one gate entry tile.
     */
    struct planned_route
    {
        /**
         * @brief Original fanin index in the node's fanin list.
         */
        uint32_t fanin_index{};
        /**
         * @brief Routed source signal from which the path is launched.
         */
        signal_type source_signal{};
        /**
         * @brief Entry tile assigned to the fanin.
         */
        tile_type entry{};
        /**
         * @brief Concrete monotone path from source to entry.
         */
        path_type path{};
    };

    /**
     * @brief Candidate launch point on an already routed signal tree.
     */
    struct branch_source
    {
        /**
         * @brief Existing layout signal at the branch point.
         */
        signal_type signal{};
        /**
         * @brief Ground-layer tile of the branch point.
         */
        tile_type tile{};
    };

    /**
     * @brief Constructor.
     *
     * @param src Source network.
     * @param p Physical design parameters.
     * @param st Statistics collector.
     */
    orthogonal_hex_impl(const Ntk& src, const orthogonal_physical_design_params& p, orthogonal_physical_design_stats& st) :
            ntk{fanout_substitution<network_type>(src)},
            fanout_ntk{ntk},
            topo_ntk{fanout_ntk},
            ps{p},
            pst{st}
    {}

    /**
     * @brief Computes the placed and routed layout.
     *
     * @return Native hexagonal orthogonal layout.
     */
    [[nodiscard]] Lyt run()
    {
        mockturtle::stopwatch stop{pst.time_total};

        compute_depths();
        compute_horizontal_slots();
        compute_output_slots();
        compute_placement_slack();
        instantiate_layout();
        reserve_blocked_tiles();
        place_primary_inputs();
        place_gates();
        place_primary_outputs();
        restore_names(ntk, layout);
        update_stats();

        return layout;
    }

    /**
     * @brief Returns the stored signal output index, if any.
     *
     * @tparam Signal Signal type.
     * @param signal Signal to inspect.
     * @return Output index, defaults to `0`.
     */
    template <typename Signal>
    [[nodiscard]] static uint32_t signal_output(const Signal& signal) noexcept
    {
        if constexpr (has_output_field<Signal>::value)
        {
            return static_cast<uint32_t>(signal.output);
        }

        return 0u;
    }

    /**
     * @brief Writes an output index into a signal, if supported by the signal type.
     *
     * @tparam Signal Signal type.
     * @param signal Signal to modify.
     * @param output_idx Output index to encode.
     */
    template <typename Signal>
    static void set_signal_output(Signal& signal, const uint32_t output_idx) noexcept
    {
        if constexpr (has_output_field<Signal>::value)
        {
            signal.output = static_cast<decltype(signal.output)>(output_idx);
        }
    }

    /**
     * @brief Returns the number of outputs a node provides.
     *
     * @param n Network node.
     * @return Number of outputs, at least `1`.
     */
    [[nodiscard]] uint32_t node_num_outputs(const mockturtle::node<network_type>& n) const noexcept
    {
        if constexpr (mockturtle::has_is_multioutput_v<network_type> && mockturtle::has_num_outputs_v<network_type>)
        {
            if (ntk.is_multioutput(n))
            {
                return ntk.num_outputs(n);
            }
        }

        return 1u;
    }

    /**
     * @brief Encodes a tile as an integer key for hash containers.
     *
     * @param t Tile to encode.
     * @return Integer key.
     */
    [[nodiscard]] static uint64_t tile_key(const tile_type& t) noexcept
    {
        return static_cast<uint64_t>(t);
    }

    /**
     * @brief Checks whether a tile is a valid in-bounds location.
     *
     * @param t Tile to inspect.
     * @return `true` iff the tile is usable.
     */
    [[nodiscard]] bool is_usable_tile(const tile_type& t) const noexcept
    {
        return !t.is_dead() && layout.is_within_bounds(t);
    }

    /**
     * @brief Returns the actual x coordinate for a horizontal slot.
     *
     * @param slot Abstract slot.
     * @return Concrete x coordinate.
     */
    [[nodiscard]] static uint64_t actual_x_from_slot(const uint64_t slot) noexcept
    {
        return x_margin + slot * slot_pitch;
    }

    /**
     * @brief Finds a free horizontal slot near a preferred one.
     *
     * @param used Slots already occupied in the current row.
     * @param preferred Preferred slot.
     * @return A free slot.
     */
    [[nodiscard]] static uint64_t nearest_free_slot(const std::set<uint64_t>& used, const uint64_t preferred) noexcept
    {
        if (used.count(preferred) == 0u)
        {
            return preferred;
        }

        for (uint64_t delta = 1u; delta <= used.size() + 1u; ++delta)
        {
            if (preferred >= delta && used.count(preferred - delta) == 0u)
            {
                return preferred - delta;
            }

            if (used.count(preferred + delta) == 0u)
            {
                return preferred + delta;
            }
        }

        return preferred + used.size() + 1u;
    }

    /**
     * @brief Enumerates candidate output slots near a preferred one.
     *
     * @param preferred Preferred slot.
     * @param used Slots already consumed by previous POs.
     * @return Candidate slots in increasing distance from `preferred`.
     */
    [[nodiscard]] std::vector<uint64_t> candidate_output_slots(const uint64_t preferred,
                                                               const std::set<uint64_t>& used) const
    {
        std::vector<uint64_t> candidates{};
        candidates.reserve(max_output_slot + 1u);

        if (preferred <= max_output_slot && used.count(preferred) == 0u)
        {
            candidates.push_back(preferred);
        }

        for (uint64_t delta = 1u; delta <= max_output_slot; ++delta)
        {
            if (preferred >= delta)
            {
                const auto lhs = preferred - delta;

                if (used.count(lhs) == 0u)
                {
                    candidates.push_back(lhs);
                }
            }

            if (preferred + delta <= max_output_slot)
            {
                const auto rhs = preferred + delta;

                if (used.count(rhs) == 0u)
                {
                    candidates.push_back(rhs);
                }
            }
        }

        return candidates;
    }

    /**
     * @brief Computes a row pitch that is sufficient for all routed connections.
     *
     * On the pointy-top hex grid, a monotone routing step advances exactly one row downward and can shift the x
     * coordinate by at most one. Therefore, the spacing between successive depth rows must be proportional to the
     * largest horizontal span any gate or PO route needs to cover, not to the total layout width.
     *
     * @return A feasible row pitch.
     */
    [[nodiscard]] uint64_t determine_row_pitch() const
    {
        uint64_t required_pitch = minimum_row_pitch;

        topo_ntk.foreach_gate(
            [this, &required_pitch](const auto& n)
            {
                const auto fc         = fanins(ntk, n);
                const auto gate_depth = node_depth[n];
                const auto gate_x     = actual_x_from_slot(node_slot[n]);

                for (const auto& fin : fc.fanin_nodes)
                {
                    const auto source_depth = node_depth[fin];
                    const auto depth_gap    = std::max<uint64_t>(1u, gate_depth - source_depth);
                    const auto source_x     = actual_x_from_slot(node_slot[fin]);
                    const auto horizontal_span =
                        static_cast<uint64_t>(gate_x > source_x ? gate_x - source_x : source_x - gate_x);
                    const auto per_gap_requirement =
                        (horizontal_span + row_pitch_slack + depth_gap - 1u) / depth_gap;

                    required_pitch = std::max(required_pitch, per_gap_requirement);
                }
            });

        ntk.foreach_po(
            [this, &required_pitch](const auto& po, const auto index)
            {
                const auto driver = ntk.get_node(po);

                if (ntk.is_constant(driver))
                {
                    return;
                }

                const auto source_depth = node_depth[driver];
                const auto depth_gap    = std::max<uint64_t>(1u, (max_gate_depth + 1u) - source_depth);
                const auto source_x     = actual_x_from_slot(node_slot[driver]);
                const auto target_x     = actual_x_from_slot(output_slot[index]);
                const auto horizontal_span =
                    static_cast<uint64_t>(target_x > source_x ? target_x - source_x : source_x - target_x);
                const auto per_gap_requirement =
                    (horizontal_span + row_pitch_slack + depth_gap - 1u) / depth_gap;

                required_pitch = std::max(required_pitch, per_gap_requirement);
            });

        return std::max(required_pitch, minimum_row_pitch);
    }

    /**
     * @brief Computes topological depths for all nodes.
     */
    void compute_depths()
    {
        topo_ntk.foreach_node(
            [this](const auto& n)
            {
                if (ntk.is_constant(n) || ntk.is_pi(n))
                {
                    node_depth[n] = 0u;
                    return;
                }

                const auto fc = fanins(ntk, n);

                uint64_t max_predecessor_depth = 0u;

                for (const auto& fi : fc.fanin_nodes)
                {
                    max_predecessor_depth = std::max(max_predecessor_depth, node_depth[fi]);
                }

                node_depth[n] = max_predecessor_depth + 1u;
                max_gate_depth = std::max(max_gate_depth, node_depth[n]);
            });
    }

    /**
     * @brief Computes one horizontal slot per non-constant node.
     */
    void compute_horizontal_slots()
    {
        std::unordered_map<uint64_t, std::set<uint64_t>> used_slots_by_depth{};

        uint64_t pi_slot = 0u;

        topo_ntk.foreach_node(
            [this, &used_slots_by_depth, &pi_slot](const auto& n)
            {
                if (ntk.is_constant(n))
                {
                    return;
                }

                if (ntk.is_pi(n))
                {
                    node_slot[n] = pi_slot;
                    used_slots_by_depth[0u].insert(pi_slot);
                    max_slot = std::max(max_slot, pi_slot);
                    ++pi_slot;
                    return;
                }

                const auto fc = fanins(ntk, n);

                uint64_t preferred_slot = used_slots_by_depth[node_depth[n]].size();

                if (!fc.fanin_nodes.empty())
                {
                    const auto sum_slots =
                        std::accumulate(fc.fanin_nodes.cbegin(), fc.fanin_nodes.cend(), uint64_t{0},
                                        [this](const auto acc, const auto& fin) { return acc + node_slot[fin]; });

                    preferred_slot = static_cast<uint64_t>(
                        std::llround(static_cast<double>(sum_slots) / static_cast<double>(fc.fanin_nodes.size())));
                }

                node_slot[n] = nearest_free_slot(used_slots_by_depth[node_depth[n]], preferred_slot);
                used_slots_by_depth[node_depth[n]].insert(node_slot[n]);
                max_slot = std::max(max_slot, node_slot[n]);
            });
    }

    /**
     * @brief Computes one horizontal slot per primary output.
     */
    void compute_output_slots()
    {
        std::set<uint64_t> used_po_slots{};

        ntk.foreach_po(
            [this, &used_po_slots](const auto& po, const auto index)
            {
                const auto driver = ntk.get_node(po);

                const uint64_t preferred_slot = ntk.is_constant(driver) ? index : node_slot[driver];

                const auto po_slot = nearest_free_slot(used_po_slots, preferred_slot);

                used_po_slots.insert(po_slot);
                output_slot.push_back(po_slot);
                max_slot = std::max(max_slot, po_slot);
            });
    }

    /**
     * @brief Computes additional placement slack for direct primary outputs.
     *
     * Cartesian orthogonal reserves structural space for output-bearing nodes while placing the logic network. The
     * native hex variant needs the same idea: rows that drive direct POs receive extra vertical slack below them, and
     * the horizontal slot search budget is widened so later gates can move around these reserved corridors.
     */
    void compute_placement_slack()
    {
        depth_row_extra.assign(max_gate_depth + 1u, 0u);

        std::unordered_set<mockturtle::node<network_type>> po_driver_nodes{};
        uint64_t                                           multioutput_po_driver_nodes{0u};

        topo_ntk.foreach_node(
            [this, &po_driver_nodes, &multioutput_po_driver_nodes](const auto& n)
            {
                if (ntk.is_constant(n) || !ntk.is_po(n))
                {
                    return;
                }

                po_driver_nodes.insert(n);

                auto& depth_extra = depth_row_extra[node_depth[n]];
                depth_extra       = std::max(depth_extra, direct_po_row_extra);

                if (node_num_outputs(n) > 1u)
                {
                    depth_extra = std::max(depth_extra, multioutput_direct_po_row_extra);
                    ++multioutput_po_driver_nodes;
                }
            });

        placement_slot_slack = po_driver_nodes.size() + multioutput_po_driver_nodes;
    }

    /**
     * @brief Creates the target layout with generous spacing for monotone routing.
     */
    void instantiate_layout()
    {
        max_output_slot = max_slot + ntk.num_pos() + placement_slot_slack;

        row_pitch = determine_row_pitch();

        if (row_pitch % 2u == 0u)
        {
            ++row_pitch;
        }

        depth_row_y.assign(max_gate_depth + 1u, 0u);

        for (uint64_t depth = 1u; depth <= max_gate_depth; ++depth)
        {
            depth_row_y[depth] = depth_row_y[depth - 1u] + row_pitch + depth_row_extra[depth - 1u];
        }

        po_row = depth_row_y[max_gate_depth] + row_pitch + depth_row_extra[max_gate_depth];

        const auto width = actual_x_from_slot(max_output_slot) + x_margin;

        layout = Lyt{{width, po_row, 1u}, row_clocking<Lyt>(ps.number_of_clock_phases)};

        topo_ntk.foreach_node(
            [this](const auto& n)
            {
                if (ntk.is_constant(n))
                {
                    return;
                }

                node_tile[n] = tile_type{actual_x_from_slot(node_slot[n]), depth_row_y[node_depth[n]], 0u};
            });
    }

    /**
     * @brief Reserves all structural tiles that routes must not consume accidentally.
     */
    void reserve_blocked_tiles()
    {
        topo_ntk.foreach_node(
            [this](const auto& n)
            {
                if (ntk.is_constant(n))
                {
                    return;
                }

                blocked_tiles.insert(tile_key(node_tile[n]));
            });
    }

    /**
     * @brief Places the reserved PI nodes on the top border.
     */
    void place_primary_inputs()
    {
        auto pi2node = reserve_input_nodes(layout, ntk);

        ntk.foreach_pi(
            [this, &pi2node](const auto& pi)
            {
                auto pi_signal = layout.move_node(pi2node[pi], node_tile[pi]);

                node_signal[pi] = {pi_signal};
            });
    }

    /**
     * @brief Places and routes all non-PI gates in topological order.
     */
    void place_gates()
    {
        topo_ntk.foreach_gate(
            [this](const auto& n)
            {
                const auto fc = fanins(ntk, n);

                if (fc.fanin_nodes.size() > 2u)
                {
                    throw high_degree_fanin_exception();
                }

                std::vector<planned_route> routes{};
                std::string                last_error{};
                const auto                 original_slot = node_slot[n];
                const auto                 original_tile = node_tile[n];

                blocked_tiles.erase(tile_key(original_tile));

                auto found_placement = false;

                for (uint64_t delta = 0u; delta <= max_output_slot && !found_placement; ++delta)
                {
                    const auto try_slot = [this, &n, &routes, &fc, &last_error, &found_placement](const uint64_t slot)
                    {
                        const auto candidate_tile =
                            tile_type{actual_x_from_slot(slot), node_tile[n].y, 0u};
                        const auto candidate_key = tile_key(candidate_tile);

                        if (blocked_tiles.count(candidate_key) != 0u)
                        {
                            return;
                        }

                        node_tile[n] = candidate_tile;
                        node_slot[n] = slot;
                        blocked_tiles.insert(candidate_key);

                        try
                        {
                            routes          = plan_gate_routes(n, fc);
                            found_placement = true;
                        }
                        catch (const std::runtime_error& e)
                        {
                            last_error = e.what();
                            blocked_tiles.erase(candidate_key);
                        }
                    };

                    if (original_slot >= delta)
                    {
                        try_slot(original_slot - delta);
                    }

                    if (!found_placement && delta != 0u && original_slot + delta <= max_output_slot)
                    {
                        try_slot(original_slot + delta);
                    }
                }

                if (!found_placement)
                {
                    node_tile[n] = original_tile;
                    node_slot[n] = original_slot;
                    blocked_tiles.insert(tile_key(original_tile));
                    throw std::runtime_error(last_error.empty() ? "orthogonal_hex could not place gate" : last_error);
                }

                const auto gate_t = node_tile[n];

                std::vector<signal_type> incoming_signals(fc.fanin_nodes.size());

                for (const auto& route : routes)
                {
                    incoming_signals[route.fanin_index] = materialize_path(route.source_signal, route.path);
                }

                signal_type gate_signal{};

                switch (incoming_signals.size())
                {
                    case 0u:
                        gate_signal = place(layout, gate_t, ntk, n);
                        break;
                    case 1u:
                        gate_signal = place(layout, gate_t, ntk, n, incoming_signals[0]);
                        break;
                    case 2u:
                        gate_signal = place(layout, gate_t, ntk, n, incoming_signals[0], incoming_signals[1],
                                            fc.constant_fanin);
                        break;
                    case 3u:
                        gate_signal = place(layout, gate_t, ntk, n, incoming_signals[0], incoming_signals[1],
                                            incoming_signals[2]);
                        break;
                    default:
                        throw high_degree_fanin_exception();
                }

                store_node_outputs(n, gate_signal);
            });
    }

    /**
     * @brief Places and routes all primary outputs on the bottom border.
     */
    void place_primary_outputs()
    {
        const auto gate_layout         = layout.clone();
        const auto initial_po_row      = po_row;
        const auto max_output_row_grow = std::max<uint64_t>(16u, 2u * (layout.x() + row_pitch + ntk.num_pos()));
        auto       last_error          = std::string{};

        for (uint64_t extra_rows = 0u; extra_rows <= max_output_row_grow; ++extra_rows)
        {
            po_row     = initial_po_row + extra_rows;
            layout     = gate_layout.clone();
            output_tile.clear();
            layout.resize({layout.x(), po_row, 1u});

            const auto placement_error = try_place_primary_outputs();

            if (!placement_error.has_value())
            {
                return;
            }

            last_error = *placement_error;
        }

        throw std::runtime_error(fmt::format(
            "orthogonal_hex could not route primary outputs to the bottom border after expanding from row {} to row "
            "{}; {}",
            initial_po_row, po_row, last_error));
    }

    /**
     * @brief Attempts to place all primary outputs on the current bottom border.
     *
     * This function materializes PO wires directly into the current layout. If routing fails for any PO, the caller is
     * expected to restore a previous layout snapshot and retry with a different bottom border row.
     *
     * @return Empty on success or a detailed error message on failure.
     */
    [[nodiscard]] std::optional<std::string> try_place_primary_outputs()
    {
        std::set<uint64_t> used_output_slots{};

        std::optional<std::string> error{};

        ntk.foreach_po(
            [this, &used_output_slots, &error](const auto& po, const auto index)
            {
                if (error.has_value())
                {
                    return;
                }

                const auto driver = ntk.get_node(po);

                std::string name{};

                if constexpr (mockturtle::has_has_output_name_v<network_type> && mockturtle::has_get_output_name_v<network_type>)
                {
                    if (ntk.has_output_name(index))
                    {
                        name = ntk.get_output_name(index);
                    }
                }

                if (ntk.is_constant(driver))
                {
                    const auto preferred_slot = output_slot[index];

                    for (const auto candidate_slot : candidate_output_slots(preferred_slot, used_output_slots))
                    {
                        const auto po_t = tile_type{actual_x_from_slot(candidate_slot), po_row, 0u};

                        layout.create_po(layout.get_constant(ntk.constant_value(driver)), name, po_t);
                        used_output_slots.insert(candidate_slot);
                        output_tile.push_back(po_t);
                        return;
                    }

                    error = "orthogonal_hex could not allocate a bottom-border slot for a constant PO";
                    return;
                }

                const auto preferred_slot    = output_slot[index];
                const auto source_signal     = source_layout_signal(po);
                const auto candidate_sources = candidate_branch_sources(source_signal);
                auto       po_path           = path_type{};
                auto       po_t              = tile_type{};
                auto       po_source_signal  = signal_type{};
                auto       routed            = false;
                std::ostringstream debug{};

                debug << "po " << index << " driver " << driver << " preferred_slot " << preferred_slot << " sources";

                for (const auto& candidate_source : candidate_sources)
                {
                    debug << " (" << candidate_source.tile.x << ", " << candidate_source.tile.y << ", "
                          << candidate_source.tile.z << ')';
                }

                debug << ':';

                for (const auto& candidate_source : candidate_sources)
                {
                    for (const auto candidate_slot : candidate_output_slots(preferred_slot, used_output_slots))
                    {
                        po_t = tile_type{actual_x_from_slot(candidate_slot), po_row, 0u};
                        debug << " src(" << candidate_source.tile.x << ", " << candidate_source.tile.y << ", "
                              << candidate_source.tile.z << ") slot " << candidate_slot << " target (" << po_t.x << ", "
                              << po_t.y << ", " << po_t.z << ") entries";

                        for (const auto& po_entry : upper_entries(po_t))
                        {
                            debug << " (" << po_entry.x << ", " << po_entry.y << ", " << po_entry.z << ")";
                            const auto candidate =
                                find_monotone_path(candidate_source.signal, candidate_source.tile, po_entry, blocked_tiles, {});
                            debug << '=' << candidate.size();

                            if (!candidate.empty())
                            {
                                po_source_signal = candidate_source.signal;
                                po_path     = candidate;
                                routed      = true;
                                break;
                            }
                        }

                        if (routed)
                        {
                            used_output_slots.insert(candidate_slot);
                            output_tile.push_back(po_t);
                            break;
                        }
                    }

                    if (routed)
                    {
                        break;
                    }
                }

                if (!routed)
                {
                    error = fmt::format("po_row {} {}", po_row, debug.str());
                    return;
                }

                layout.create_po(materialize_path(po_source_signal, po_path), name, po_t);
            });

        return error;
    }

    /**
 * @brief Returns the legal upper entry tiles of a pointy-top gate.
     *
     * @param gate_t Gate tile.
     * @return North-west and north-east entries.
     */
    [[nodiscard]] std::vector<tile_type> upper_entries(const tile_type& gate_t) const noexcept
    {
        std::vector<tile_type> entries{};

        for (const auto& entry : std::array<tile_type, 2u>{layout.north_west(gate_t), layout.north_east(gate_t)})
        {
            if (is_usable_tile(entry) &&
                std::none_of(entries.cbegin(), entries.cend(), [&entry](const auto& existing) { return existing == entry; }))
            {
                entries.push_back(entry);
            }
        }

        return entries;
    }

    /**
     * @brief Checks whether a routed signal can launch another downward branch from a tile.
     *
     * @param source_tile Tile that would serve as branch source.
     * @return `true` iff at least one legal lower neighbor can be used.
     */
    [[nodiscard]] bool has_downward_launch_capacity(const tile_type& source_tile) const noexcept
    {
        for (const auto& successor : std::array<tile_type, 2u>{layout.south_west(source_tile), layout.south_east(source_tile)})
        {
            if (successor == source_tile || !is_usable_tile(successor))
            {
                continue;
            }

            if (blocked_tiles.count(tile_key(successor)) != 0u)
            {
                continue;
            }

            if (is_projected_tile_empty(successor))
            {
                if (has_projected_step_capacity(source_tile, successor))
                {
                    return true;
                }

                continue;
            }

            if (has_projected_step_capacity(source_tile, successor) && has_available_crossing_layer(successor) &&
                is_crossable_successor(source_tile, layout.below(successor)))
            {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Checks whether a pointy-top wire tile currently carries a north-west to south-east connection.
     *
     * @param t Tile to inspect.
     * @return `true` iff the tile contains a `NW->SE` wire segment.
     */
    [[nodiscard]] bool has_nw_se_orientation(const tile_type& t) const noexcept
    {
        return layout.has_north_western_incoming_signal(t) || layout.has_south_eastern_outgoing_signal(t);
    }

    /**
     * @brief Checks whether a pointy-top wire tile currently carries a north-east to south-west connection.
     *
     * @param t Tile to inspect.
     * @return `true` iff the tile contains a `NE->SW` wire segment.
     */
    [[nodiscard]] bool has_ne_sw_orientation(const tile_type& t) const noexcept
    {
        return layout.has_north_eastern_incoming_signal(t) || layout.has_south_western_outgoing_signal(t);
    }

    /**
     * @brief Evaluates a predicate on all occupied z-layers that project to the same hex tile.
     *
     * @tparam Predicate Predicate type.
     * @param t Tile whose projected `(x, y)` position is inspected.
     * @param predicate Predicate evaluated on each occupied layer at that position.
     * @return `true` iff the predicate matches on at least one occupied layer.
     */
    template <typename Predicate>
    [[nodiscard]] bool any_projected_occupant_satisfies(const tile_type& t, Predicate&& predicate) const noexcept
    {
        const auto projected = layout.below(t);

        if (!layout.is_empty_tile(projected) && predicate(projected))
        {
            return true;
        }

        if (const auto elevated = layout.above(projected);
            elevated != projected && !layout.is_empty_tile(elevated) && predicate(elevated))
        {
            return true;
        }

        return false;
    }

    /**
     * @brief Checks whether both z-layers of the projected tile are empty.
     *
     * @param t Tile whose projected position is inspected.
     * @return `true` iff neither the ground nor the crossing layer is occupied there.
     */
    [[nodiscard]] bool is_projected_tile_empty(const tile_type& t) const noexcept
    {
        const auto projected = layout.below(t);
        return layout.is_empty_tile(projected) && layout.is_empty_tile(layout.above(projected));
    }

    /**
     * @brief Checks whether the projected tile already uses its north-west input side.
     *
     * @param t Tile whose projected position is inspected.
     * @return `true` iff any stacked occupant already has a north-west incoming connection.
     */
    [[nodiscard]] bool has_projected_north_west_incoming(const tile_type& t) const noexcept
    {
        const auto projected        = layout.below(t);
        const auto expected_neighbor = layout.north_west(projected);

        return any_projected_occupant_satisfies(
            projected,
            [this, &expected_neighbor](const auto& occupant)
            {
                for (const auto& incoming : layout.incoming_data_flow(occupant))
                {
                    if (layout.below(static_cast<tile_type>(incoming)) == expected_neighbor)
                    {
                        return true;
                    }
                }

                return false;
            });
    }

    /**
     * @brief Checks whether the projected tile already uses its north-east input side.
     *
     * @param t Tile whose projected position is inspected.
     * @return `true` iff any stacked occupant already has a north-east incoming connection.
     */
    [[nodiscard]] bool has_projected_north_east_incoming(const tile_type& t) const noexcept
    {
        const auto projected        = layout.below(t);
        const auto expected_neighbor = layout.north_east(projected);

        return any_projected_occupant_satisfies(
            projected,
            [this, &expected_neighbor](const auto& occupant)
            {
                for (const auto& incoming : layout.incoming_data_flow(occupant))
                {
                    if (layout.below(static_cast<tile_type>(incoming)) == expected_neighbor)
                    {
                        return true;
                    }
                }

                return false;
            });
    }

    /**
     * @brief Checks whether the projected tile already uses its south-west output side.
     *
     * @param t Tile whose projected position is inspected.
     * @return `true` iff any stacked occupant already has a south-west outgoing connection.
     */
    [[nodiscard]] bool has_projected_south_west_outgoing(const tile_type& t) const noexcept
    {
        const auto projected        = layout.below(t);
        const auto expected_neighbor = layout.south_west(projected);

        return any_projected_occupant_satisfies(
            projected,
            [this, &expected_neighbor](const auto& occupant)
            {
                for (const auto& outgoing : layout.outgoing_data_flow(occupant))
                {
                    if (layout.below(static_cast<tile_type>(outgoing)) == expected_neighbor)
                    {
                        return true;
                    }
                }

                return false;
            });
    }

    /**
     * @brief Checks whether the projected tile already uses its south-east output side.
     *
     * @param t Tile whose projected position is inspected.
     * @return `true` iff any stacked occupant already has a south-east outgoing connection.
     */
    [[nodiscard]] bool has_projected_south_east_outgoing(const tile_type& t) const noexcept
    {
        const auto projected        = layout.below(t);
        const auto expected_neighbor = layout.south_east(projected);

        return any_projected_occupant_satisfies(
            projected,
            [this, &expected_neighbor](const auto& occupant)
            {
                for (const auto& outgoing : layout.outgoing_data_flow(occupant))
                {
                    if (layout.below(static_cast<tile_type>(outgoing)) == expected_neighbor)
                    {
                        return true;
                    }
                }

                return false;
            });
    }

    /**
     * @brief Checks whether a new routed step still has free projected source and target sides.
     *
     * A step to `south_east(current)` consumes the current tile's `SE` output side and the successor tile's `NW`
     * input side. A step to `south_west(current)` analogously consumes `SW` and `NE`.
     *
     * @param current Current path tile.
     * @param successor Candidate successor tile.
     * @return `true` iff the step would not reuse an already occupied projected side.
     */
    [[nodiscard]] bool has_projected_step_capacity(const tile_type& current, const tile_type& successor) const noexcept
    {
        const auto current_projected   = layout.below(current);
        const auto successor_projected = layout.below(successor);

        if (successor_projected == layout.south_east(current_projected))
        {
            return !has_projected_south_east_outgoing(current_projected) &&
                   !has_projected_north_west_incoming(successor_projected);
        }

        if (successor_projected == layout.south_west(current_projected))
        {
            return !has_projected_south_west_outgoing(current_projected) &&
                   !has_projected_north_east_incoming(successor_projected);
        }

        return false;
    }

    /**
     * @brief Decides whether an occupied successor tile already continues the same routed signal.
     *
     * This allows the path finder to traverse previously materialized wire branches of the same net before branching
     * off again later, instead of mistaking them for foreign obstructions. Logic tiles are intentionally excluded:
     * multiple outputs may share the same source tile, so geometric adjacency alone is not enough to prove that a
     * successor belongs to the same routed signal.
     *
     * @param current Current path tile.
     * @param successor Candidate occupied successor tile on the ground layer.
     * @return `true` iff the successor is already a direct fanout of `current`.
     */
    [[nodiscard]] bool is_reusable_successor(const tile_type& current, const tile_type& successor) const noexcept
    {
        if (layout.is_empty_tile(current))
        {
            return false;
        }

        const auto current_node = layout.get_node(current);

        if (!layout.is_wire(current_node))
        {
            return false;
        }

        for (const auto& candidate : std::array<tile_type, 3u>{successor, layout.above(successor), layout.below(successor)})
        {
            if (!is_usable_tile(candidate) || layout.is_empty_tile(candidate))
            {
                continue;
            }

            const auto candidate_signal = static_cast<signal_type>(candidate);
            const auto current_signal   = static_cast<signal_type>(current);

            if (layout.is_outgoing_signal(current, candidate_signal) && layout.is_incoming_signal(candidate, current_signal))
            {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Checks whether the appropriate crossing layer is available above an occupied projected tile.
     *
     * When the path uses the ground-layer coordinate of an occupied projected tile, the alternative crossing layer is
     * `above(projected)`. When the path already uses the crossing-layer coordinate, that exact tile must be empty.
     *
     * @param successor Candidate successor tile of the path.
     * @return `true` iff the route can be materialized on the non-occupied layer of that projected tile.
     */
    [[nodiscard]] bool has_available_crossing_layer(const tile_type& successor) const noexcept
    {
        const auto successor_projected = layout.below(successor);

        if (layout.is_empty_tile(successor_projected))
        {
            return false;
        }

        if (successor == successor_projected)
        {
            return layout.is_empty_tile(layout.above(successor_projected));
        }

        return layout.is_empty_tile(successor);
    }

    /**
     * @brief Enforces distinct launch sides for direct routes from multi-output gates.
     *
     * The first routed segment leaving a two-output gate is output-sensitive: output `0` launches on the gate's
     * south-east edge and output `1` launches on the south-west edge. Once a signal has left the source gate, later
     * branches are handled by regular wire-side capacity checks on the routed tree.
     *
     * @param source_signal Routed source signal.
     * @param source Root tile from which the current path was launched.
     * @param current Current path tile.
     * @param successor Candidate successor tile.
     * @param source_node Optional source-network node if the source tile is not materialized in the layout yet.
     * @return `true` iff the current step respects the source gate's output-side assignment.
     */
    [[nodiscard]] bool uses_legal_output_launch_side(const signal_type& source_signal, const tile_type& source,
                                                     const tile_type& current, const tile_type& successor,
                                                     const std::optional<mockturtle::node<network_type>>& source_node =
                                                         std::nullopt) const noexcept
    {
        if (current != source)
        {
            return true;
        }

        const auto launch_node = source_node.has_value() ? *source_node : layout.get_node(source);

        if (!ntk.is_multioutput(launch_node) || node_num_outputs(launch_node) < 2u)
        {
            return true;
        }

        const auto source_projected    = layout.below(source);
        const auto successor_projected = layout.below(successor);

        switch (signal_output(source_signal))
        {
            case 0u:
                return successor_projected == layout.south_east(source_projected);
            case 1u:
                return successor_projected == layout.south_west(source_projected);
            default:
                return false;
        }
    }

    /**
     * @brief Decides whether an occupied projected successor tile can host a legal pointy-top crossing.
     *
     * Only proper wire tiles are crossable, never fanouts or I/O tiles. Moreover, the existing wire orientation must be
     * opposite to the one induced by the current routing step: a `SE` step may only cross a `NE->SW` wire and a `SW`
     * step may only cross a `NW->SE` wire.
     *
     * @param current Current path tile.
     * @param successor_projected Occupied successor tile on the projected ground layer.
     * @return `true` iff the projected successor can host a legal crossing.
     */
    [[nodiscard]] bool is_crossable_successor(const tile_type& current, const tile_type& successor_projected) const noexcept
    {
        const auto successor_node = layout.get_node(successor_projected);

        if (!layout.is_wire(successor_node) || layout.is_fanout(successor_node) || layout.is_pi(successor_node) ||
            layout.is_po(successor_node))
        {
            return false;
        }

        const auto current_projected = layout.below(current);

        if (successor_projected == layout.south_east(current_projected))
        {
            return has_ne_sw_orientation(successor_projected) && !has_nw_se_orientation(successor_projected);
        }

        if (successor_projected == layout.south_west(current_projected))
        {
            return has_nw_se_orientation(successor_projected) && !has_ne_sw_orientation(successor_projected);
        }

        return false;
    }

    /**
     * @brief Collects alternative launch points along an already routed signal tree.
     *
     * @param source_signal Layout signal that drives the routed tree.
     * @return Candidate branch sources ordered from bottom to top.
     */
    [[nodiscard]] std::vector<branch_source> candidate_branch_sources(const signal_type& source_signal) const
    {
        std::vector<branch_source>       candidates{};
        std::queue<signal_type>          frontier{};
        std::vector<bool>                visited(layout.size(), false);

        const auto root      = layout.get_node(source_signal);
        const auto root_tile = layout.get_tile(root);

        const auto is_matching_fanin = [this](const auto fanout_node, const auto& expected_signal)
        {
            auto matches = false;

            layout.foreach_fanin(
                fanout_node,
                [this, &expected_signal, &matches](const auto& fanin)
                {
                    if (layout.get_node(fanin) == layout.get_node(expected_signal) &&
                        signal_output(fanin) == signal_output(expected_signal))
                    {
                        matches = true;
                        return false;
                    }

                    return true;
                });

            return matches;
        };

        frontier.push(source_signal);
        visited[root] = true;

        candidates.push_back({source_signal, root_tile});

        while (!frontier.empty())
        {
            const auto current_signal = frontier.front();
            const auto current        = layout.get_node(current_signal);
            const auto current_tile   = layout.get_tile(current);
            frontier.pop();

            if (current != root && has_downward_launch_capacity(current_tile))
            {
                candidates.push_back({current_signal, current_tile});
            }

            for (const auto& successor_base : std::array<tile_type, 2u>{layout.south_west(current_tile),
                                                                        layout.south_east(current_tile)})
            {
                for (const auto& successor : std::array<tile_type, 3u>{successor_base, layout.above(successor_base),
                                                                       layout.below(successor_base)})
                {
                    if (!is_usable_tile(successor) || layout.is_empty_tile(successor))
                    {
                        continue;
                    }

                    const auto successor_node = layout.get_node(successor);

                    if (visited[successor_node] || layout.is_po(successor_node))
                    {
                        continue;
                    }

                    if ((layout.is_wire(successor_node) || layout.is_fanout(successor_node)) &&
                        is_matching_fanin(successor_node, current_signal))
                    {
                        visited[successor_node] = true;
                        frontier.push(layout.make_signal(successor_node));
                    }
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& lhs, const auto& rhs)
                  {
                      if (lhs.tile.y != rhs.tile.y)
                      {
                          return lhs.tile.y > rhs.tile.y;
                      }

                      if (lhs.tile.x != rhs.tile.x)
                      {
                          return lhs.tile.x < rhs.tile.x;
                      }

                      return lhs.tile.z < rhs.tile.z;
                  });

        candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                     [](const auto& lhs, const auto& rhs) { return lhs.tile == rhs.tile; }),
                         candidates.end());

        return candidates;
    }

    /**
     * @brief Plans distinct monotone routes for all fanins of one gate.
     *
     * @param n Gate node.
     * @param fc Fanin container of `n`.
     * @return One planned route per non-constant fanin.
     */
    [[nodiscard]] std::vector<planned_route> plan_gate_routes(const mockturtle::node<network_type>& n,
                                                              const fanin_container<network_type>&   fc) const
    {
        const auto gate_t    = node_tile[n];
        const auto entries   = upper_entries(gate_t);
        const auto num_fanin = static_cast<uint32_t>(fc.fanin_signals.size());

        if (num_fanin == 0u)
        {
            return {};
        }

        if (num_fanin > entries.size())
        {
            throw std::runtime_error("orthogonal_hex could not assign enough distinct upper gate entries");
        }

        std::vector<std::vector<branch_source>> source_candidates_by_fanin(num_fanin);

        for (uint32_t i = 0u; i < num_fanin; ++i)
        {
            source_candidates_by_fanin[i] = candidate_branch_sources(source_layout_signal(fc.fanin_signals[i]));
        }

        std::vector<uint32_t> candidate_indices(entries.size());
        std::iota(candidate_indices.begin(), candidate_indices.end(), 0u);
        std::vector<std::vector<uint32_t>> assignments{};

        std::sort(candidate_indices.begin(), candidate_indices.end());

        do
        {
            assignments.emplace_back(candidate_indices.begin(), candidate_indices.begin() + num_fanin);
        } while (std::next_permutation(candidate_indices.begin(), candidate_indices.end()));

        const auto route_cost = [&entries, &source_candidates_by_fanin](const auto& assignment)
        {
            uint64_t cost = 0u;

            for (uint32_t i = 0u; i < assignment.size(); ++i)
            {
                const auto entry_t  = entries[assignment[i]];
                auto       best_cost = std::numeric_limits<uint64_t>::max();

                for (const auto& candidate_source : source_candidates_by_fanin[i])
                {
                    const auto horizontal_offset = static_cast<uint64_t>(
                        candidate_source.tile.x > entry_t.x ? candidate_source.tile.x - entry_t.x : entry_t.x - candidate_source.tile.x);
                    const auto vertical_offset = static_cast<uint64_t>(
                        candidate_source.tile.y > entry_t.y ? candidate_source.tile.y - entry_t.y : entry_t.y - candidate_source.tile.y);

                    best_cost = std::min(best_cost, horizontal_offset + vertical_offset);
                }

                cost += best_cost;
            }

            return cost;
        };

        std::sort(assignments.begin(), assignments.end(),
                  [&route_cost](const auto& a, const auto& b) { return route_cost(a) < route_cost(b); });

        std::ostringstream debug{};

        debug << "gate " << n << " at (" << gate_t.x << ", " << gate_t.y << ", " << gate_t.z << ") entries:";
        for (const auto& entry : entries)
        {
            debug << " (" << entry.x << ", " << entry.y << ", " << entry.z << ")";
        }

        for (const auto& assignment : assignments)
        {
            const auto route_span = [this, &assignment, &entries, &fc](const auto fanin_index)
            {
                const auto source_t = layout.get_tile(layout.get_node(source_layout_signal(fc.fanin_signals[fanin_index])));
                const auto entry_t  = entries[assignment[fanin_index]];

                return static_cast<uint64_t>(source_t.x > entry_t.x ? source_t.x - entry_t.x : entry_t.x - source_t.x) +
                       entry_t.y - source_t.y;
            };

            std::vector<uint32_t> routing_order(num_fanin);
            std::iota(routing_order.begin(), routing_order.end(), 0u);

            std::vector<std::vector<uint32_t>> routing_orders{};

            do
            {
                routing_orders.push_back(routing_order);
            } while (std::next_permutation(routing_order.begin(), routing_order.end()));

            std::sort(routing_orders.begin(), routing_orders.end(),
                      [&route_span](const auto& lhs, const auto& rhs)
                      {
                          return route_span(lhs.front()) > route_span(rhs.front());
                      });

            for (const auto& current_routing_order : routing_orders)
            {
                std::vector<planned_route> planned_routes{};
                planned_routes.reserve(num_fanin);
                std::unordered_set<uint64_t> temporary_blocked{};
                std::unordered_set<uint64_t> temporary_north_west_incoming{};
                std::unordered_set<uint64_t> temporary_north_east_incoming{};
                std::unordered_set<uint64_t> temporary_south_west_outgoing{};
                std::unordered_set<uint64_t> temporary_south_east_outgoing{};

                auto valid_assignment = true;

                debug << " assignment";
                for (const auto idx : assignment)
                {
                    debug << ' ' << idx;
                }
                debug << " order";
                for (const auto idx : current_routing_order)
                {
                    debug << ' ' << idx;
                }
                debug << ':';

                for (const auto fanin_index : current_routing_order)
                {
                    const auto entry_t = entries[assignment[fanin_index]];
                    auto       path    = path_type{};
                    std::optional<branch_source> selected_source{};

                    auto candidate_sources = source_candidates_by_fanin[fanin_index];

                    std::sort(candidate_sources.begin(), candidate_sources.end(),
                              [&entry_t](const auto& lhs, const auto& rhs)
                              {
                                  const auto lhs_vertical = entry_t.y - lhs.tile.y;
                                  const auto rhs_vertical = entry_t.y - rhs.tile.y;

                                  if (lhs_vertical != rhs_vertical)
                                  {
                                      return lhs_vertical < rhs_vertical;
                                  }

                                  const auto lhs_horizontal = lhs.tile.x > entry_t.x ? lhs.tile.x - entry_t.x : entry_t.x - lhs.tile.x;
                                  const auto rhs_horizontal = rhs.tile.x > entry_t.x ? rhs.tile.x - entry_t.x : entry_t.x - rhs.tile.x;

                                  if (lhs_horizontal != rhs_horizontal)
                                  {
                                      return lhs_horizontal < rhs_horizontal;
                                  }

                                  return lhs.tile.x < rhs.tile.x;
                              });

                    for (const auto& candidate_source : candidate_sources)
                    {
                        path = find_monotone_path(candidate_source.signal, candidate_source.tile, entry_t, blocked_tiles,
                                                 temporary_blocked);

                        debug << " fi" << fanin_index << " (" << candidate_source.tile.x << ", " << candidate_source.tile.y
                              << ", " << candidate_source.tile.z << ")->(" << entry_t.x << ", " << entry_t.y << ", "
                              << entry_t.z << ")=" << path.size();

                        if (!path.empty())
                        {
                            selected_source = candidate_source;
                            break;
                        }
                    }

                    if (!selected_source.has_value())
                    {
                        valid_assignment = false;
                        break;
                    }

                    const auto reserve_projected_step = [this, &temporary_north_west_incoming, &temporary_north_east_incoming,
                                                         &temporary_south_west_outgoing,
                                                         &temporary_south_east_outgoing](const auto& current,
                                                                                         const auto& successor)
                    {
                        const auto current_projected_key   = tile_key(layout.below(current));
                        const auto successor_projected_key = tile_key(layout.below(successor));
                        const auto current_projected       = layout.below(current);
                        const auto successor_projected     = layout.below(successor);

                        if (successor_projected == layout.south_east(current_projected))
                        {
                            if (has_projected_south_east_outgoing(current_projected) ||
                                has_projected_north_west_incoming(successor_projected) ||
                                temporary_south_east_outgoing.count(current_projected_key) != 0u ||
                                temporary_north_west_incoming.count(successor_projected_key) != 0u)
                            {
                                return false;
                            }

                            temporary_south_east_outgoing.insert(current_projected_key);
                            temporary_north_west_incoming.insert(successor_projected_key);
                            return true;
                        }

                        if (successor_projected == layout.south_west(current_projected))
                        {
                            if (has_projected_south_west_outgoing(current_projected) ||
                                has_projected_north_east_incoming(successor_projected) ||
                                temporary_south_west_outgoing.count(current_projected_key) != 0u ||
                                temporary_north_east_incoming.count(successor_projected_key) != 0u)
                            {
                                return false;
                            }

                            temporary_south_west_outgoing.insert(current_projected_key);
                            temporary_north_east_incoming.insert(successor_projected_key);
                            return true;
                        }

                        return false;
                    };

                    for (auto it = path.cbegin(); std::next(it) != path.cend(); ++it)
                    {
                        if (!reserve_projected_step(*it, *std::next(it)))
                        {
                            valid_assignment = false;
                            break;
                        }
                    }

                    if (!valid_assignment)
                    {
                        break;
                    }

                    for (auto it = path.cbegin() + 1; it != path.cend(); ++it)
                    {
                        temporary_blocked.insert(tile_key(layout.below(*it)));
                    }

                    planned_routes.push_back({fanin_index, selected_source->signal, entry_t, path});
                }

                if (valid_assignment)
                {
                    return planned_routes;
                }
            }
        }

        throw std::runtime_error(fmt::format("orthogonal_hex could not route gate {} at tile ({}, {}, {}) with {} "
                                             "fanins; {}",
                                             n, gate_t.x, gate_t.y, gate_t.z, num_fanin, debug.str()));
    }

    /**
 * @brief Finds a downward-only path on the pointy-top hex grid.
     *
     * @param source_signal Routed signal launched from `source`.
     * @param source Start tile.
     * @param target End tile.
     * @param hard_blocked Structurally blocked tiles.
     * @param soft_blocked Temporarily blocked projected tiles for the current planning step.
     * @param source_node Optional source-network node if `source` is not materialized in the layout yet.
     * @return Path from `source` to `target`, or an empty path if none was found.
     */
    [[nodiscard]] path_type find_monotone_path(
        const signal_type& source_signal, const tile_type& source, const tile_type& target,
        const std::unordered_set<uint64_t>& hard_blocked, const std::unordered_set<uint64_t>& soft_blocked,
        const std::optional<mockturtle::node<network_type>>& source_node = std::nullopt) const
    {
        if (source == target)
        {
            return path_type{source};
        }

        std::queue<tile_type> frontier{};
        std::unordered_map<uint64_t, uint64_t> parent{};
        std::unordered_set<uint64_t> visited{};

        frontier.push(source);
        visited.insert(tile_key(source));
        parent.emplace(tile_key(source), tile_key(source));

        auto found = false;

        while (!frontier.empty() && !found)
        {
            const auto current = frontier.front();
            frontier.pop();

            const auto successors = std::array<tile_type, 2u>{layout.south_east(current), layout.south_west(current)};

            for (const auto& successor : successors)
            {
                if (successor == current || !is_usable_tile(successor))
                {
                    continue;
                }

                if (successor.y > target.y)
                {
                    continue;
                }

                const auto successor_key = tile_key(successor);

                if (visited.count(successor_key) != 0u)
                {
                    continue;
                }

                if (const auto successor_projected_key = tile_key(layout.below(successor));
                    soft_blocked.count(successor_projected_key) != 0u &&
                    layout.below(successor) != layout.below(target))
                {
                    continue;
                }

                if (!uses_legal_output_launch_side(source_signal, source, current, successor, source_node))
                {
                    continue;
                }

                if (!is_routable_successor(current, successor, target, hard_blocked))
                {
                    continue;
                }

                visited.insert(successor_key);
                parent.emplace(successor_key, tile_key(current));

                if (successor == target)
                {
                    found = true;
                    break;
                }

                frontier.push(successor);
            }
        }

        if (!found)
        {
            return {};
        }

        std::vector<tile_type> reversed_path{};

        for (auto current_key = tile_key(target);; current_key = parent.at(current_key))
        {
            reversed_path.emplace_back(tile_type{current_key});

            if (current_key == tile_key(source))
            {
                break;
            }
        }

        path_type path{};
        path.reserve(reversed_path.size());

        for (auto it = reversed_path.crbegin(); it != reversed_path.crend(); ++it)
        {
            path.append(*it);
        }

        return path;
    }

    /**
     * @brief Checks whether a successor tile may be used in a monotone path.
     *
     * @param current Current path tile.
     * @param successor Candidate successor tile.
     * @param target Final target tile.
     * @param hard_blocked Structurally blocked tiles.
     * @return `true` iff the successor is admissible.
     */
    [[nodiscard]] bool is_routable_successor(const tile_type& current, const tile_type& successor, const tile_type& target,
                                             const std::unordered_set<uint64_t>& hard_blocked) const
    {
        const auto successor_key = tile_key(successor);

        if (hard_blocked.count(successor_key) != 0u && successor != target)
        {
            return false;
        }

        if (is_projected_tile_empty(successor))
        {
            return has_projected_step_capacity(current, successor);
        }

        if (!has_projected_step_capacity(current, successor))
        {
            return false;
        }

        if (!has_available_crossing_layer(successor))
        {
            return false;
        }

        return is_crossable_successor(current, layout.below(successor));
    }

    /**
     * @brief Materializes a planned path as buffers in the layout.
     *
     * @param source_signal Signal that drives the path.
     * @param path Path to instantiate.
     * @return Signal located at the path's end tile.
     */
    [[nodiscard]] signal_type materialize_path(const signal_type& source_signal, const path_type& path)
    {
        auto routed_signal = source_signal;

        for (auto it = path.cbegin() + 1; it != path.cend(); ++it)
        {
            if (const auto existing_signal = existing_connected_successor_signal(routed_signal, *it);
                existing_signal.has_value())
            {
                routed_signal = *existing_signal;
                continue;
            }

            const auto place_t = layout.is_empty_tile(*it) ? *it : layout.above(*it);
            routed_signal      = layout.create_buf(routed_signal, place_t);
        }

        return routed_signal;
    }

    /**
     * @brief Reuses an already connected successor signal if the path follows an existing routed branch.
     *
     * Native hex PO branching may start from an already routed wire or fanout tile. In that case, the shortest path to
     * the border can legitimately traverse already materialized wire tiles of the same net. Reusing that signal avoids
     * stacking a second buffer above the existing one, which would otherwise turn a simple continuation into a bogus
     * crossing tile.
     *
     * @param current_signal Current routed signal.
     * @param successor_base Ground-layer successor tile from the planned path.
     * @return Existing successor signal if the net is already connected there.
     */
    [[nodiscard]] std::optional<signal_type> existing_connected_successor_signal(const signal_type& current_signal,
                                                                                 const tile_type&   successor_base) const
    {
        const auto has_matching_fanin = [this, &current_signal](const auto candidate_node)
        {
            auto matches = false;

            layout.foreach_fanin(
                candidate_node,
                [this, &current_signal, &matches](const auto& fanin)
                {
                    if (layout.get_node(fanin) == layout.get_node(current_signal) &&
                        signal_output(fanin) == signal_output(current_signal))
                    {
                        matches = true;
                        return false;
                    }

                    return true;
                });

            return matches;
        };

        for (const auto& candidate : std::array<tile_type, 3u>{successor_base, layout.above(successor_base),
                                                               layout.below(successor_base)})
        {
            if (!is_usable_tile(candidate) || layout.is_empty_tile(candidate))
            {
                continue;
            }

            const auto candidate_node = layout.get_node(candidate);

            if (has_matching_fanin(candidate_node))
            {
                return layout.make_signal(candidate_node);
            }
        }

        return std::nullopt;
    }

    /**
     * @brief Looks up the placed layout signal for a source network signal.
     *
     * @param source_signal Source network signal.
     * @return Corresponding layout signal.
     */
    [[nodiscard]] signal_type source_layout_signal(const mockturtle::signal<network_type>& source_signal) const
    {
        const auto source_node  = ntk.get_node(source_signal);
        const auto source_index = signal_output(source_signal);

        return node_signal[source_node][source_index];
    }

    /**
     * @brief Stores all outputs of a placed node.
     *
     * @param n Network node.
     * @param base_signal Base layout signal returned by the placement call.
     */
    void store_node_outputs(const mockturtle::node<network_type>& n, const signal_type& base_signal)
    {
        const auto num_outputs = node_num_outputs(n);
        auto&      signals     = node_signal[n];

        signals.clear();
        signals.reserve(num_outputs);

        for (uint32_t output_idx = 0u; output_idx < num_outputs; ++output_idx)
        {
            auto signal = base_signal;
            set_signal_output(signal, output_idx);
            signals.push_back(signal);
        }
    }

    /**
     * @brief Updates the public statistics object.
     */
    void update_stats() noexcept
    {
        pst.x_size        = layout.x() + 1u;
        pst.y_size        = layout.y() + 1u;
        pst.num_gates     = layout.num_gates();
        pst.num_wires     = layout.num_wires();
        pst.num_crossings = layout.num_crossings();
    }

    /**
     * @brief Horizontal spacing between abstract slots.
     */
    static constexpr uint64_t slot_pitch = 1u;
    /**
     * @brief Left and right border margin.
     */
    static constexpr uint64_t x_margin = 1u;
    /**
     * @brief Minimum spacing between successive depth rows.
     */
    static constexpr uint64_t minimum_row_pitch = 3u;
    /**
     * @brief Additional vertical slack for monotone hex routing.
     */
    static constexpr uint64_t row_pitch_slack = 2u;
    /**
     * @brief Additional rows reserved below depths that drive at least one direct primary output.
     */
    static constexpr uint64_t direct_po_row_extra = 1u;
    /**
     * @brief Additional rows reserved below depths that contain a multi-output direct-PO driver.
     */
    static constexpr uint64_t multioutput_direct_po_row_extra = 2u;
    /**
     * @brief Converted and normalized working network.
     */
    network_type ntk;
    /**
     * @brief Fanout view of `ntk`.
     */
    fanout_network_type fanout_ntk;
    /**
     * @brief Topological traversal view of `ntk`.
     */
    topo_network_type topo_ntk;
    /**
     * @brief Physical design parameters.
     */
    orthogonal_physical_design_params ps;
    /**
     * @brief Statistics collector.
     */
    orthogonal_physical_design_stats& pst;
    /**
     * @brief Result layout under construction.
     */
    Lyt layout{};
    /**
     * @brief Topological depth per node.
     */
    mockturtle::node_map<uint64_t, network_type> node_depth{ntk};
    /**
     * @brief Horizontal slot per node.
     */
    mockturtle::node_map<uint64_t, network_type> node_slot{ntk};
    /**
     * @brief Concrete tile per placed node.
     */
    mockturtle::node_map<tile_type, network_type> node_tile{ntk};
    /**
     * @brief One or two concrete layout signals per placed node.
     */
    mockturtle::node_map<std::vector<signal_type>, network_type> node_signal{ntk};
    /**
     * @brief Absolute y coordinate per topological depth after output-aware row expansion.
     */
    std::vector<uint64_t> depth_row_y{};
    /**
     * @brief Additional vertical slack inserted below each depth.
     */
    std::vector<uint64_t> depth_row_extra{};
    /**
     * @brief Abstract output slot per PO.
     */
    std::vector<uint64_t> output_slot{};
    /**
     * @brief Concrete PO tile per output.
     */
    std::vector<tile_type> output_tile{};
    /**
     * @brief Tiles reserved for nodes and gate/PO entry points.
     */
    std::unordered_set<uint64_t> blocked_tiles{};
    /**
     * @brief Maximum gate depth.
     */
    uint64_t max_gate_depth{0u};
    /**
     * @brief Maximum horizontal slot used by any node or output.
     */
    uint64_t max_slot{0u};
    /**
     * @brief Additional horizontal slots reserved for output-aware detours during placement.
     */
    uint64_t placement_slot_slack{0u};
    /**
     * @brief Maximum bottom-border slot that may be used for PO placement.
     */
    uint64_t max_output_slot{0u};
    /**
     * @brief Vertical spacing between consecutive logic levels.
     */
    uint64_t row_pitch{0u};
    /**
     * @brief Y coordinate of the southern PO border.
     */
    uint64_t po_row{0u};

  public:
    /**
     * @brief Runs the native hexagonal orthogonal implementation.
     *
     * @param src Source network.
     * @param p Physical design parameters.
     * @param st Statistics collector.
     * @return Hexagonal gate-level layout.
     */
    [[nodiscard]] static Lyt apply(const Ntk& src, const orthogonal_physical_design_params& p,
                                   orthogonal_physical_design_stats& st)
    {
        orthogonal_hex_impl impl{src, p, st};
        return impl.run();
    }
};

}  // namespace detail

/**
 * @brief Native hexagonal orthogonal physical design with strict top-down constraints.
 *
 * This variant is intentionally separate from cartesian `orthogonal`. It enforces the same
 * global I/O orientation as native hexagonal `gold`: all primary inputs are placed on the top
 * border, all primary outputs on the bottom border, and all signal flow is routed monotonically
 * downwards on the pointy-top hexagonal grid.
 *
 * The strict top-down variant is intentionally restricted to binary gates. In the pointy-top
 * Bestagon model used here, every tile exposes two legal incoming ports (`NORTH_WEST`,
 * `NORTH_EAST`) and two legal outgoing ports (`SOUTH_WEST`, `SOUTH_EAST`).
 *
 * @tparam Lyt Desired native hexagonal gate-level layout type.
 * @tparam Ntk Specification network type.
 * @param ntk Specification network.
 * @param ps Physical design parameters.
 * @param pst Optional statistics object.
 * @return Hexagonal gate-level layout implementing `ntk`.
 */
template <typename Lyt, typename Ntk>
Lyt orthogonal_hex(const Ntk& ntk, orthogonal_physical_design_params ps = {},
                   orthogonal_physical_design_stats* pst = nullptr)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<typename Lyt::base_type>, "Lyt is not based on a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "orthogonal_hex requires pointy-top hexagonal layouts");
    static_assert(mockturtle::is_network_type_v<Ntk>, "Ntk is not a network type");

    if (has_high_degree_fanin_nodes(ntk, 2u))
    {
        throw high_degree_fanin_exception();
    }

    orthogonal_physical_design_stats st{};

    auto result = detail::orthogonal_hex_impl<Lyt, Ntk>::apply(ntk, ps, st);

    if (pst != nullptr)
    {
        *pst = st;
    }

    return result;
}

}  // namespace fiction

#endif  // FICTION_ORTHOGONAL_HEX_HPP
