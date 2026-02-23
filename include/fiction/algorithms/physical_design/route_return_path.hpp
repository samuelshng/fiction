//
// Created by marcel on 23.02.26.
//

#ifndef FICTION_ROUTE_RETURN_PATH_HPP
#define FICTION_ROUTE_RETURN_PATH_HPP

#include "fiction/algorithms/path_finding/a_star.hpp"
#include "fiction/layouts/clocking_scheme.hpp"
#include "fiction/layouts/obstruction_layout.hpp"
#include "fiction/traits.hpp"

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
#include <unordered_set>
#include <utility>
#include <vector>

namespace fiction
{

/**
 * @brief Coordinate used to describe routing whitelist entries.
 */
struct route_return_path_coordinate
{
    /**
     * @brief x-coordinate.
     */
    uint64_t x{};
    /**
     * @brief y-coordinate.
     */
    uint64_t y{};
    /**
     * @brief z-coordinate.
     */
    uint64_t z{};
};

/**
 * @brief Parameters for return-path routing.
 */
struct route_return_path_params
{
    /**
     * @brief Requested number of rows above the shifted original layout.
     *
     * The effective top margin is at least the number of routed PO-PI pairs.
     */
    uint32_t top_margin = 0u;
    /**
     * @brief Requested number of rows below the shifted original layout.
     *
     * The effective bottom margin is at least what is required by the lane spacing and number of routed pairs.
     */
    uint32_t bottom_margin = 0u;
    /**
     * @brief Requested number of columns to the right of the original layout.
     *
     * The effective right margin is at least what is required by the lane spacing and number of routed pairs.
     */
    uint32_t right_margin = 0u;
    /**
     * @brief Spacing between neighboring return-path lanes.
     */
    uint32_t lane_spacing = 2u;
    /**
     * @brief Optional whitelist of coordinates that routing is allowed to use.
     *
     * If empty, routing can use any unobstructed coordinate (current default behavior).
     * If non-empty, all non-whitelisted coordinates are blocked. Occupied coordinates (e.g., gates, wires, PIs, POs)
     * remain blocked even when listed here.
     */
    std::vector<route_return_path_coordinate> routing_whitelist{};
    /**
     * @brief Optional explicit order in which PO-PI pairs are routed.
     *
     * Entries are pair indices `i` for the fixed pair mapping `PO[i] -> PI[i]`.
     * If empty, the default inner-to-outer ordering is used.
     */
    std::vector<uint32_t> pin_routing_order{};
    /**
     * @brief Allows the final segment into the PI target to violate clock-validity.
     *
     * If set to `true`, only the final segment may be clock-invalid while all preceding segments are routed as usual.
     * If set to `false`, the final segment is routed clock-aware and must be clock-valid at the PI target.
     */
    bool allow_invalid_final_segment_clocking = true;
};

/**
 * @brief Statistics for return-path routing.
 */
struct route_return_path_stats
{
    /**
     * @brief Total runtime of the algorithm.
     */
    mockturtle::stopwatch<>::duration time_total{0};
    /**
     * @brief Number of routed PO-PI return pairs.
     */
    uint32_t num_routed_pairs = 0u;
    /**
     * @brief Number of routed path segments across all pairs.
     */
    uint32_t num_routed_segments = 0u;

    /**
     * @brief Reports collected runtime statistics.
     *
     * @param out Output stream.
     */
    void report(std::ostream& out = std::cout) const
    {
        out << fmt::format("[i] total time = {:.2f} secs\n", mockturtle::to_seconds(time_total));
        out << fmt::format("[i] routed pairs = {}\n", num_routed_pairs);
        out << fmt::format("[i] routed segments = {}\n", num_routed_segments);
    }
};

namespace detail
{

/**
 * @brief Computes the effective top margin for the return corridor.
 *
 * @param num_pairs Number of routed PO-PI pairs.
 * @param params Routing parameters.
 * @return Effective top margin.
 */
[[nodiscard]] inline uint32_t determine_required_top_margin(const uint32_t                  num_pairs,
                                                            const route_return_path_params& params) noexcept
{
    return std::max(params.top_margin, num_pairs);
}

/**
 * @brief Computes an effective lane-based margin for the return corridor.
 *
 * @param num_pairs Number of routed PO-PI pairs.
 * @param lane_spacing Spacing between neighboring lanes.
 * @param requested_margin User-requested minimum margin.
 * @return Effective margin.
 */
[[nodiscard]] inline uint32_t determine_required_lane_margin(const uint32_t num_pairs, const uint32_t lane_spacing,
                                                             const uint32_t requested_margin) noexcept
{
    if (num_pairs == 0u)
    {
        return requested_margin;
    }

    const uint32_t required = 1u + (num_pairs - 1u) * lane_spacing;
    return std::max(requested_margin, required);
}

/**
 * @brief Creates a new layout with additional top, bottom, and right margins.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Original layout.
 * @param top_margin Top margin in rows.
 * @param bottom_margin Bottom margin in rows.
 * @param right_margin Right margin in columns.
 * @return Extended layout with preserved clocking scheme and layout name.
 */
template <typename Lyt>
[[nodiscard]] Lyt create_extended_layout(const Lyt& lyt, const uint32_t top_margin, const uint32_t bottom_margin,
                                         const uint32_t right_margin)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    const auto              crossing_layer = std::max(lyt.z(), static_cast<decltype(lyt.z())>(1));
    const aspect_ratio<Lyt> new_ar{lyt.x() + right_margin, lyt.y() + top_margin + bottom_margin, crossing_layer};

    return Lyt{new_ar, lyt.get_clocking_scheme(), lyt.get_layout_name()};
}

/**
 * @brief Copies a layout into another layout with a vertical offset while preserving PI and PO semantics.
 *
 * @tparam SrcLyt Source layout type.
 * @tparam DstLyt Destination layout type.
 * @param original_lyt Source layout.
 * @param target_lyt Destination layout.
 * @param y_offset Vertical shift to apply.
 */
template <typename SrcLyt, typename DstLyt>
void copy_layout_with_vertical_offset(const SrcLyt& original_lyt, DstLyt& target_lyt, const uint32_t y_offset)
{
    static_assert(is_gate_level_layout_v<SrcLyt>, "SrcLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<SrcLyt>, "SrcLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<SrcLyt>, "SrcLyt does not have pointy-top hexagonal orientation");
    static_assert(is_gate_level_layout_v<DstLyt>, "DstLyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<DstLyt>, "DstLyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<DstLyt>, "DstLyt does not have pointy-top hexagonal orientation");

    mockturtle::node_map<mockturtle::signal<DstLyt>, SrcLyt> node2signal{original_lyt};

    node2signal[original_lyt.get_node(original_lyt.get_constant(false))] = target_lyt.get_constant(false);
    node2signal[original_lyt.get_node(original_lyt.get_constant(true))]  = target_lyt.get_constant(true);

    // Copy PIs first such that all subsequent fanin reconstruction can reference them.
    original_lyt.foreach_pi(
        [&original_lyt, &target_lyt, &node2signal, y_offset](const auto& pi)
        {
            const auto         pi_tile = original_lyt.get_tile(pi);
            const tile<DstLyt> shifted_pi_tile{pi_tile.x, pi_tile.y + y_offset, pi_tile.z};
            const auto         new_pi = target_lyt.create_pi(original_lyt.get_name(pi), shifted_pi_tile);
            node2signal[pi]           = new_pi;
        });

    // Copy all non-PI and non-PO gates/wires.
    original_lyt.foreach_gate(
        [&](const auto& node)
        {
            if (original_lyt.is_po(node))
            {
                return;
            }

            std::vector<mockturtle::signal<DstLyt>> new_children{};
            new_children.reserve(original_lyt.fanin_size(node));

            const auto fanin_collector = [&](const auto& fanin_signal)
            {
                const auto fanin_node = original_lyt.get_node(fanin_signal);
                auto       new_signal = node2signal[fanin_node];
                if (original_lyt.is_complemented(fanin_signal))
                {
                    new_signal = !new_signal;
                }
                new_children.push_back(new_signal);
            };

            original_lyt.template foreach_fanin<decltype(fanin_collector), false>(node, std::move(fanin_collector));

            const auto         node_tile = original_lyt.get_tile(node);
            const tile<DstLyt> shifted_node_tile{node_tile.x, node_tile.y + y_offset, node_tile.z};

            node2signal[node] =
                target_lyt.create_node(new_children, original_lyt.node_function(node), shifted_node_tile);
        });

    // Recreate POs at shifted coordinates with preserved names and fanins.
    original_lyt.foreach_po(
        [&](const auto& po, const auto index)
        {
            const auto po_node = original_lyt.get_node(po);

            mockturtle::signal<DstLyt> po_driver = target_lyt.get_constant(false);
            bool                       has_fanin = false;

            const auto fanin_collector = [&](const auto& fanin_signal)
            {
                if (has_fanin)
                {
                    return;
                }

                const auto fanin_node = original_lyt.get_node(fanin_signal);
                po_driver             = node2signal[fanin_node];
                if (original_lyt.is_complemented(fanin_signal))
                {
                    po_driver = !po_driver;
                }

                has_fanin = true;
            };

            original_lyt.template foreach_fanin<decltype(fanin_collector), false>(po_node, std::move(fanin_collector));

            const auto         po_tile = original_lyt.get_tile(po_node);
            const tile<DstLyt> shifted_po_tile{po_tile.x, po_tile.y + y_offset, po_tile.z};

            target_lyt.create_po(po_driver, original_lyt.get_output_name(static_cast<uint32_t>(index)),
                                 shifted_po_tile);
        });
}

/**
 * @brief Assigns explicit clock overrides in the newly added return corridors.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Working layout.
 * @param core_x Maximum x-coordinate of the shifted original layout.
 * @param core_y Original maximum y-coordinate of the unshifted layout.
 * @param top_margin Top margin used to shift the original layout.
 */
template <typename Lyt>
void assign_corridor_clock_numbers(Lyt& lyt, const uint32_t core_x, const uint32_t core_y, const uint32_t top_margin)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    const auto core_bottom = top_margin + core_y;

    for (uint64_t y = 0; y <= lyt.y(); ++y)
    {
        for (uint64_t x = 0; x <= lyt.x(); ++x)
        {
            const bool in_top_corridor    = y < top_margin;
            const bool in_bottom_corridor = y > core_bottom;
            const bool in_right_corridor  = x > core_x;

            if (in_top_corridor || in_bottom_corridor || in_right_corridor)
            {
                const auto clock_number =
                    static_cast<typename Lyt::clock_number_t>((x + y) % static_cast<uint64_t>(lyt.num_clocks()));
                lyt.assign_clock_number({x, y}, clock_number);
            }
        }
    }
}

/**
 * @brief A routed PO-PI pair description.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct return_route_pair
{
    /**
     * @brief Source PO tile.
     */
    tile<Lyt> source{};
    /**
     * @brief Target PI tile.
     */
    tile<Lyt> target{};
    /**
     * @brief Pair index in PI/PO order.
     */
    uint32_t index = 0u;
    /**
     * @brief Lane index assigned by routing order.
     */
    uint32_t lane = 0u;
};

/**
 * @brief Creates deterministic PO-PI route pairs in index order.
 *
 * Pairing is index-based: PO[i] is connected to PI[i].
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Working layout.
 * @param num_pairs Number of pairs to create.
 * @return Vector of route pairs in index order.
 */
template <typename Lyt>
[[nodiscard]] std::vector<return_route_pair<Lyt>> create_return_route_pairs(const Lyt& lyt, const uint32_t num_pairs)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    std::vector<return_route_pair<Lyt>> pairs{};
    pairs.reserve(num_pairs);

    for (uint32_t i = 0u; i < num_pairs; ++i)
    {
        pairs.push_back({static_cast<tile<Lyt>>(lyt.po_at(i)), lyt.get_tile(lyt.pi_at(i)), i, 0u});
    }

    return pairs;
}

/**
 * @brief Applies an explicit user-provided routing order to PO-PI pairs.
 *
 * @tparam Lyt Gate-level layout type.
 * @param pairs Route pairs in index order.
 * @param pin_routing_order User-provided pair order.
 * @return Reordered pairs.
 */
template <typename Lyt>
[[nodiscard]] std::vector<return_route_pair<Lyt>>
apply_user_pin_routing_order(std::vector<return_route_pair<Lyt>> pairs, const std::vector<uint32_t>& pin_routing_order)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    if (pairs.empty() || pin_routing_order.empty())
    {
        return pairs;
    }

    assert(pin_routing_order.size() == pairs.size() && "pin_routing_order must list each pair index exactly once");

    std::vector<bool> seen_pair(pairs.size(), false);

    std::vector<return_route_pair<Lyt>> ordered_pairs{};
    ordered_pairs.reserve(pairs.size());

    for (const auto pair_idx : pin_routing_order)
    {
        assert(pair_idx < pairs.size() && "pin_routing_order contains an out-of-range pair index");
        assert(!seen_pair[pair_idx] && "pin_routing_order contains duplicate pair indices");

        ordered_pairs.push_back(pairs[pair_idx]);
        seen_pair[pair_idx] = true;
    }

    return ordered_pairs;
}

/**
 * @brief Applies default inner-to-outer ordering to PO-PI route pairs.
 *
 * @tparam Lyt Gate-level layout type.
 * @param pairs Route pairs.
 * @return Inner-to-outer ordered pairs.
 */
template <typename Lyt>
[[nodiscard]] std::vector<return_route_pair<Lyt>>
apply_default_inner_to_outer_order(std::vector<return_route_pair<Lyt>> pairs)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    if (pairs.empty())
    {
        return pairs;
    }

    const auto [min_it, max_it] = std::minmax_element(pairs.cbegin(), pairs.cend(), [](const auto& lhs, const auto& rhs)
                                                      { return lhs.target.x < rhs.target.x; });
    const double center_x       = (static_cast<double>(min_it->target.x) + static_cast<double>(max_it->target.x)) / 2.0;

    std::stable_sort(pairs.begin(), pairs.end(),
                     [center_x](const auto& lhs, const auto& rhs)
                     {
                         const auto lhs_dist = std::abs(static_cast<double>(lhs.target.x) - center_x);
                         const auto rhs_dist = std::abs(static_cast<double>(rhs.target.x) - center_x);
                         if (lhs_dist == rhs_dist)
                         {
                             if (lhs.target.x == rhs.target.x)
                             {
                                 return lhs.index < rhs.index;
                             }
                             return lhs.target.x < rhs.target.x;
                         }
                         return lhs_dist < rhs_dist;
                     });

    return pairs;
}

/**
 * @brief Assigns lane indices according to pair order.
 *
 * @tparam Lyt Gate-level layout type.
 * @param pairs Route pairs in routing order.
 * @return Route pairs with assigned lane indices.
 */
template <typename Lyt>
[[nodiscard]] std::vector<return_route_pair<Lyt>> assign_route_lanes(std::vector<return_route_pair<Lyt>> pairs)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    for (uint32_t lane = 0u; lane < pairs.size(); ++lane)
    {
        pairs[lane].lane = lane;
    }

    return pairs;
}

/**
 * @brief Creates deterministic PO-PI route pairs and assigns lane indices according to routing order.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Working layout.
 * @param num_pairs Number of pairs to create.
 * @param pin_routing_order Optional explicit pair order.
 * @return Route pairs with lane assignments.
 */
template <typename Lyt>
[[nodiscard]] std::vector<return_route_pair<Lyt>>
create_ordered_return_route_pairs(const Lyt& lyt, const uint32_t num_pairs,
                                  const std::vector<uint32_t>& pin_routing_order = {})
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    auto pairs = create_return_route_pairs(lyt, num_pairs);

    if (!pin_routing_order.empty())
    {
        pairs = apply_user_pin_routing_order(std::move(pairs), pin_routing_order);
    }
    else
    {
        pairs = apply_default_inner_to_outer_order(std::move(pairs));
    }

    return assign_route_lanes(std::move(pairs));
}

/**
 * @brief Creates deterministic routing waypoints for a single return pair.
 *
 * @tparam Lyt Gate-level layout type.
 * @param pair Return pair description.
 * @param core_x Maximum x-coordinate of the shifted original layout.
 * @param core_y Original maximum y-coordinate of the unshifted layout.
 * @param top_margin Top margin used to shift the original layout.
 * @param lane_spacing Lane spacing.
 * @return Ordered waypoint list ending in the PI target.
 */
template <typename Lyt>
[[nodiscard]] std::vector<tile<Lyt>>
create_return_route_waypoints(const return_route_pair<Lyt>& pair, const uint32_t core_x, const uint32_t core_y,
                              const uint32_t top_margin, const uint32_t lane_spacing)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    const auto core_bottom = top_margin + core_y;
    const auto lane_y      = core_bottom + 1u + pair.lane * lane_spacing;
    const auto lane_x      = core_x + 1u + pair.lane * lane_spacing;
    const auto top_lane_y  = top_margin - 1u - pair.lane;

    const tile<Lyt> bottom_anchor{pair.source.x, lane_y, pair.source.z};
    const tile<Lyt> right_lower_anchor{lane_x, lane_y, pair.source.z};
    const tile<Lyt> right_upper_anchor{lane_x, top_lane_y, pair.source.z};
    const tile<Lyt> top_anchor{pair.target.x, top_lane_y, pair.target.z};

    return {bottom_anchor, right_lower_anchor, right_upper_anchor, top_anchor, pair.target};
}

/**
 * @brief Creates a temporary unrestricted clocking scheme for path search.
 *
 * @tparam Lyt Gate-level layout type.
 * @return Clocking scheme with one phase, allowing movement to every adjacent coordinate.
 */
template <typename Lyt>
[[nodiscard]] typename Lyt::clocking_scheme_t create_unrestricted_clocking_scheme()
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    using clocking_scheme_t = typename Lyt::clocking_scheme_t;
    using clock_zone_t      = typename Lyt::clock_zone;

    const typename clocking_scheme_t::clock_function unrestricted_clock_function =
        []([[maybe_unused]] const clock_zone_t& cz) noexcept { return typename clocking_scheme_t::clock_number{0u}; };

    return clocking_scheme_t{"RETURN_PATH_ROUTING",
                             unrestricted_clock_function,
                             static_cast<typename clocking_scheme_t::degree>(Lyt::max_fanin_size),
                             static_cast<typename clocking_scheme_t::degree>(Lyt::max_fanin_size),
                             1u,
                             true};
}

/**
 * @brief Finds a path between two coordinates using A* and planar routing constraints.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Working layout.
 * @param objective Source-target objective.
 * @param use_unrestricted_clocking If true, runs A* with a temporary unrestricted one-phase clocking scheme.
 * @return Found path, or empty path if none exists.
 */
template <typename Lyt>
[[nodiscard]] layout_coordinate_path<Lyt> find_a_star_path(Lyt& lyt, const routing_objective<Lyt>& objective,
                                                           const bool use_unrestricted_clocking = true)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    const auto stored_clocking = lyt.get_clocking_scheme();

    if (use_unrestricted_clocking)
    {
        lyt.replace_clocking_scheme(create_unrestricted_clocking_scheme<Lyt>());
    }

    a_star_params params{};
    params.crossings = false;

    const auto path = a_star<layout_coordinate_path<Lyt>>(lyt, objective, euclidean_distance_functor<Lyt>(),
                                                          unit_cost_functor<Lyt>(), params);

    if (use_unrestricted_clocking)
    {
        lyt.replace_clocking_scheme(stored_clocking);
    }

    return path;
}

/**
 * @brief Assigns a local clock gradient along a routed path.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Path Path type.
 * @param lyt Working layout.
 * @param path Routed path.
 * @param keep_existing_target_clock If true and the path target is occupied, the target's current clock is preserved.
 */
template <typename Lyt, typename Path>
void assign_path_clock_gradient(Lyt& lyt, const Path& path, const bool keep_existing_target_clock = true)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    if (path.size() < 2u)
    {
        return;
    }

    auto current_clock = lyt.get_clock_number(path.source());

    for (auto it = std::next(path.cbegin()); it != path.cend(); ++it)
    {
        const bool is_last = std::next(it) == path.cend();

        if (is_last && keep_existing_target_clock && !lyt.is_empty_tile(*it))
        {
            break;
        }

        current_clock =
            static_cast<typename Lyt::clock_number_t>((current_clock + 1u) % static_cast<uint64_t>(lyt.num_clocks()));
        lyt.assign_clock_number(*it, current_clock);
    }
}

/**
 * @brief Materializes a routed path segment and returns the resulting signal at the segment end.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Path Path type.
 * @param lyt Working layout.
 * @param source_signal Driving signal at the segment source.
 * @param path Path segment from source to endpoint.
 * @param connect_to_existing_target If true and the endpoint is occupied, connect to that endpoint node.
 * @return Signal at the segment endpoint.
 */
template <typename Lyt, typename Path>
[[nodiscard]] mockturtle::signal<Lyt> materialize_path_segment(Lyt& lyt, const mockturtle::signal<Lyt>& source_signal,
                                                               const Path& path, const bool connect_to_existing_target)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    if (path.size() < 2u)
    {
        assert(path.size() >= 2u && "Path segment must contain at least source and target");
        throw std::logic_error{"Path segment must contain at least source and target"};
    }

    auto incoming_signal = source_signal;

    for (auto it = std::next(path.cbegin()); it != path.cend(); ++it)
    {
        const auto& coord   = *it;
        const bool  is_last = std::next(it) == path.cend();

        if (is_last && connect_to_existing_target && !lyt.is_empty_tile(coord))
        {
            lyt.connect(incoming_signal, lyt.get_node(coord));
            return lyt.make_signal(lyt.get_node(coord));
        }

        incoming_signal = lyt.create_buf(incoming_signal, lyt.is_empty_tile(coord) ? coord : lyt.above(coord));
    }

    return incoming_signal;
}

/**
 * @brief Applies routing whitelist restrictions as explicit coordinate obstructions.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Working obstruction-aware layout.
 * @param routing_whitelist Allowed routing coordinates.
 * @param route_pairs Ordered route pairs.
 * @param route_waypoints Waypoints for each pair in `route_pairs`.
 */
template <typename Lyt>
void apply_routing_whitelist_obstructions(Lyt& lyt, const std::vector<route_return_path_coordinate>& routing_whitelist,
                                          const std::vector<return_route_pair<Lyt>>& route_pairs,
                                          const std::vector<std::vector<tile<Lyt>>>& route_waypoints)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    if (routing_whitelist.empty())
    {
        return;
    }

    std::unordered_set<uint64_t> allowed_coordinates{};
    allowed_coordinates.reserve(routing_whitelist.size() + route_pairs.size() * 6u);

    const auto add_if_within_bounds = [&lyt, &allowed_coordinates](const tile<Lyt>& t)
    {
        if (lyt.is_within_bounds(t))
        {
            allowed_coordinates.insert(static_cast<uint64_t>(t));
        }
    };

    for (const auto& c : routing_whitelist)
    {
        add_if_within_bounds(tile<Lyt>{c.x, c.y, c.z});
    }

    // Always allow route segment endpoints, even if they are not listed in the whitelist.
    for (uint64_t i = 0u; i < route_pairs.size(); ++i)
    {
        add_if_within_bounds(route_pairs[i].source);
        add_if_within_bounds(route_pairs[i].target);

        for (const auto& waypoint : route_waypoints[i])
        {
            add_if_within_bounds(waypoint);
        }
    }

    for (uint64_t z = 0u; z <= lyt.z(); ++z)
    {
        for (uint64_t y = 0u; y <= lyt.y(); ++y)
        {
            for (uint64_t x = 0u; x <= lyt.x(); ++x)
            {
                const tile<Lyt> t{x, y, z};
                if (allowed_coordinates.count(static_cast<uint64_t>(t)) == 0u)
                {
                    lyt.obstruct_coordinate(t);
                }
            }
        }
    }
}

/**
 * @brief Implementation class for return-path routing.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
class route_return_path_impl
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

  public:
    /**
     * @brief Standard constructor.
     *
     * @param lyt Input layout.
     * @param p Parameters.
     * @param st Statistics.
     */
    route_return_path_impl(const Lyt& lyt, const route_return_path_params& p, route_return_path_stats& st) :
            layout{lyt},
            params{p},
            pst{st}
    {}

    /**
     * @brief Executes return-path routing.
     *
     * @return Layout with routed return paths.
     */
    [[nodiscard]] Lyt run()
    {
        mockturtle::stopwatch stop{pst.time_total};

        assert(params.lane_spacing > 0u && "lane_spacing must be greater than 0");

        const auto num_pairs = static_cast<uint32_t>(std::min(layout.num_pis(), layout.num_pos()));

        if (num_pairs == 0u)
        {
            return layout;
        }

        const uint32_t top_margin = determine_required_top_margin(num_pairs, params);
        const uint32_t bottom_margin =
            determine_required_lane_margin(num_pairs, params.lane_spacing, params.bottom_margin);
        const uint32_t right_margin =
            determine_required_lane_margin(num_pairs, params.lane_spacing, params.right_margin);

        using routing_layout = obstruction_layout<Lyt>;

        routing_layout routed_layout{create_extended_layout(layout, top_margin, bottom_margin, right_margin)};

        copy_layout_with_vertical_offset(layout, routed_layout, top_margin);
        assign_corridor_clock_numbers(routed_layout, static_cast<uint32_t>(layout.x()),
                                      static_cast<uint32_t>(layout.y()), top_margin);

        const auto route_pairs = create_ordered_return_route_pairs(routed_layout, num_pairs, params.pin_routing_order);

        std::vector<std::vector<tile<routing_layout>>> route_waypoints{};
        route_waypoints.reserve(route_pairs.size());

        for (const auto& pair : route_pairs)
        {
            route_waypoints.push_back(create_return_route_waypoints(pair, static_cast<uint32_t>(layout.x()),
                                                                    static_cast<uint32_t>(layout.y()), top_margin,
                                                                    params.lane_spacing));
        }

        apply_routing_whitelist_obstructions(routed_layout, params.routing_whitelist, route_pairs, route_waypoints);

        for (uint64_t pair_idx = 0u; pair_idx < route_pairs.size(); ++pair_idx)
        {
            const auto& pair      = route_pairs[pair_idx];
            const auto& waypoints = route_waypoints[pair_idx];

            auto current_signal = routed_layout.make_signal(routed_layout.get_node(pair.source));
            auto current_source = pair.source;

            for (uint32_t waypoint_idx = 0u; waypoint_idx < waypoints.size(); ++waypoint_idx)
            {
                const auto target_waypoint = waypoints[waypoint_idx];
                if (current_source == target_waypoint)
                {
                    continue;
                }

                const bool connect_to_existing_target = waypoint_idx == waypoints.size() - 1u;
                const bool require_clock_valid_final_segment =
                    connect_to_existing_target && !params.allow_invalid_final_segment_clocking;

                const routing_objective<routing_layout> objective{current_source, target_waypoint};
                const auto path = find_a_star_path(routed_layout, objective, !require_clock_valid_final_segment);

                assert(!path.empty() && "A* failed to find a mandatory return-path segment");
                if (path.empty())
                {
                    throw std::logic_error{"A* failed to find a mandatory return-path segment"};
                }

                assert(path.target() == target_waypoint && "Routed segment does not end at requested waypoint");
                if (path.target() != target_waypoint)
                {
                    throw std::logic_error{"Routed segment does not end at requested waypoint"};
                }

                if (!require_clock_valid_final_segment)
                {
                    assign_path_clock_gradient(routed_layout, path, true);
                }

                if (connect_to_existing_target)
                {
                    const bool degenerate_final_segment = path.size() < 2u || path.source() == path.target();
                    assert(!degenerate_final_segment && "Final return-path segment to PI must be non-degenerate");
                    if (degenerate_final_segment)
                    {
                        throw std::logic_error{"Final return-path segment to PI must be non-degenerate"};
                    }
                }

                current_signal =
                    materialize_path_segment(routed_layout, current_signal, path, connect_to_existing_target);
                current_source = static_cast<tile<routing_layout>>(current_signal);

                ++pst.num_routed_segments;
            }

            const auto pi_node = routed_layout.pi_at(pair.index);

            assert(routed_layout.get_tile(pi_node) == pair.target && "Pair target must remain aligned with PI tile");
            if (routed_layout.get_tile(pi_node) != pair.target)
            {
                throw std::logic_error{"Pair target is no longer aligned with PI tile"};
            }

            assert(current_source == pair.target && "Return-path routing did not end at PI target tile");
            if (current_source != pair.target)
            {
                throw std::logic_error{"Return-path routing did not end at PI target tile"};
            }

            const bool target_has_fanin = routed_layout.template fanin_size<false>(pi_node) > 0u;
            assert(target_has_fanin && "Return-path target PI must have at least one incoming fanin");
            if (!target_has_fanin)
            {
                throw std::logic_error{"Return-path target PI must have at least one incoming fanin"};
            }

            if (!params.allow_invalid_final_segment_clocking)
            {
                const bool target_has_clock_valid_fanin = routed_layout.fanin_size(pi_node) > 0u;
                assert(target_has_clock_valid_fanin && "Final return-path segment must be clock-valid at PI target");
                if (!target_has_clock_valid_fanin)
                {
                    throw std::logic_error{"Final return-path segment must be clock-valid at PI target"};
                }
            }

            ++pst.num_routed_pairs;
        }

        return static_cast<Lyt>(routed_layout);
    }

  private:
    /**
     * @brief Input layout.
     */
    const Lyt& layout;
    /**
     * @brief Parameters.
     */
    route_return_path_params params;
    /**
     * @brief Statistics.
     */
    route_return_path_stats& pst;
};

}  // namespace detail

/**
 * @brief Routes return paths from primary outputs back to primary inputs.
 *
 * This algorithm assumes a pointy-top hexagonal row-clocked layout and performs deterministic inner-to-outer routing
 * through explicitly allocated top, bottom, and right return corridors. Pair routing order can be explicitly specified
 * via @ref route_return_path_params::pin_routing_order or defaults to inner-to-outer ordering.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Input layout.
 * @param ps Parameters.
 * @param pst Optional statistics pointer.
 * @return Layout with routed return paths.
 */
template <typename Lyt>
[[nodiscard]] Lyt route_return_path(const Lyt& lyt, route_return_path_params ps = {},
                                    route_return_path_stats* pst = nullptr)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    assert(lyt.is_clocking_scheme(clock_name::ROW) && "Layout must be row-wise clocked");

    route_return_path_stats             st{};
    detail::route_return_path_impl<Lyt> p{lyt, ps, st};

    const auto result = p.run();

    if (pst != nullptr)
    {
        *pst = st;
    }

    return result;
}

}  // namespace fiction

#endif  // FICTION_ROUTE_RETURN_PATH_HPP
