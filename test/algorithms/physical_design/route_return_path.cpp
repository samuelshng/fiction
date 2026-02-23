//
// Created by marcel on 23.02.26.
//

#include <catch2/catch_test_macros.hpp>

#include <fiction/algorithms/physical_design/route_return_path.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/obstruction_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>

#include <unordered_set>

using namespace fiction;

namespace
{

template <typename Lyt>
Lyt create_two_pi_two_po_layout()
{
    Lyt lyt{{8, 6, 1}, row_clocking<Lyt>()};

    const auto a = lyt.create_pi("a", {1, 0});
    const auto b = lyt.create_pi("b", {5, 0});

    const auto ab = lyt.create_and(a, b, {3, 1});
    const auto w1 = lyt.create_buf(ab, {2, 2});
    const auto w2 = lyt.create_buf(ab, {4, 2});

    lyt.create_po(w1, "f0", {1, 4});
    lyt.create_po(w2, "f1", {5, 4});

    return lyt;
}

template <typename Lyt>
Lyt create_single_pi_single_po_layout()
{
    Lyt lyt{{6, 4, 1}, row_clocking<Lyt>()};

    const auto a = lyt.create_pi("a", {2, 0});
    lyt.create_po(a, "f", {2, 3});

    return lyt;
}

template <typename Lyt>
Lyt create_single_pi_single_po_u_layout()
{
    Lyt lyt{{8, 4, 1}, row_clocking<Lyt>()};

    const auto a = lyt.create_pi("a", {1, 0});
    lyt.create_po(a, "f", {6, 3});

    return lyt;
}

template <typename Lyt>
Lyt create_five_pi_five_po_layout()
{
    Lyt lyt{{12, 6, 1}, row_clocking<Lyt>()};

    const auto a = lyt.create_pi("a", {1, 0});
    const auto b = lyt.create_pi("b", {3, 0});
    const auto c = lyt.create_pi("c", {5, 0});
    const auto d = lyt.create_pi("d", {7, 0});
    const auto e = lyt.create_pi("e", {9, 0});

    lyt.create_po(e, "e", {1, 4});
    lyt.create_po(d, "d", {3, 4});
    lyt.create_po(c, "c", {5, 4});
    lyt.create_po(b, "b", {7, 4});
    lyt.create_po(a, "a", {9, 4});

    return lyt;
}

template <typename Lyt>
std::vector<route_return_path_coordinate> build_whitelist_from_planned_routes(const Lyt&               layout,
                                                                              route_return_path_params params)
{
    const auto num_pairs  = static_cast<uint32_t>(std::min(layout.num_pis(), layout.num_pos()));
    const auto top_margin = detail::determine_required_top_margin(num_pairs, params);
    const auto bottom_margin =
        detail::determine_required_lane_margin(num_pairs, params.lane_spacing, params.bottom_margin);
    const auto right_margin =
        detail::determine_required_lane_margin(num_pairs, params.lane_spacing, params.right_margin);

    using routing_layout = obstruction_layout<Lyt>;

    routing_layout routing_lyt{detail::create_extended_layout(layout, top_margin, bottom_margin, right_margin)};
    detail::copy_layout_with_vertical_offset(layout, routing_lyt, top_margin);

    const auto route_pairs =
        detail::create_ordered_return_route_pairs(routing_lyt, num_pairs, params.pin_routing_order);

    std::vector<route_return_path_coordinate> whitelist{};

    for (const auto& pair : route_pairs)
    {
        const auto waypoints =
            detail::create_return_route_waypoints(pair, static_cast<uint32_t>(layout.x()),
                                                  static_cast<uint32_t>(layout.y()), top_margin, params.lane_spacing);

        auto current_source = pair.source;

        for (const auto& waypoint : waypoints)
        {
            if (current_source == waypoint)
            {
                continue;
            }

            const auto path =
                detail::find_a_star_path(routing_lyt, routing_objective<routing_layout>{current_source, waypoint});
            REQUIRE(!path.empty());

            for (const auto& c : path)
            {
                whitelist.push_back({c.x, c.y, c.z});
            }

            current_source = waypoint;
        }
    }

    return whitelist;
}

template <typename Lyt>
std::unordered_set<uint64_t> whitelist_as_tile_ids(const std::vector<route_return_path_coordinate>& whitelist)
{
    std::unordered_set<uint64_t> tile_ids{};
    tile_ids.reserve(whitelist.size());

    for (const auto& c : whitelist)
    {
        tile_ids.insert(static_cast<uint64_t>(tile<Lyt>{c.x, c.y, c.z}));
    }

    return tile_ids;
}

}  // namespace

TEST_CASE("Route return path helper functions", "[route-return-path]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    const auto layout = create_two_pi_two_po_layout<gate_layout>();

    SECTION("Margin calculations")
    {
        route_return_path_params params{};
        params.top_margin    = 0u;
        params.bottom_margin = 0u;
        params.right_margin  = 0u;
        params.lane_spacing  = 2u;

        CHECK(detail::determine_required_top_margin(2u, params) == 2u);
        CHECK(detail::determine_required_lane_margin(2u, params.lane_spacing, params.bottom_margin) == 3u);
        CHECK(detail::determine_required_lane_margin(3u, params.lane_spacing, params.right_margin) == 5u);
    }

    SECTION("Layout extension and shifted copy")
    {
        constexpr uint32_t top_margin    = 2u;
        constexpr uint32_t bottom_margin = 3u;
        constexpr uint32_t right_margin  = 4u;

        const auto extended = detail::create_extended_layout(layout, top_margin, bottom_margin, right_margin);
        CHECK(extended.x() == layout.x() + right_margin);
        CHECK(extended.y() == layout.y() + top_margin + bottom_margin);
        CHECK(extended.z() == std::max(layout.z(), static_cast<decltype(layout.z())>(1)));

        obstruction_layout<gate_layout> shifted_layout{extended};
        detail::copy_layout_with_vertical_offset(layout, shifted_layout, top_margin);

        CHECK(shifted_layout.num_pis() == layout.num_pis());
        CHECK(shifted_layout.num_pos() == layout.num_pos());

        const auto original_pi_tile = layout.get_tile(layout.pi_at(0u));
        const auto shifted_pi_tile  = shifted_layout.get_tile(shifted_layout.pi_at(0u));
        CHECK(shifted_pi_tile.x == original_pi_tile.x);
        CHECK(shifted_pi_tile.y == original_pi_tile.y + top_margin);
    }

    SECTION("Corridor clock assignment")
    {
        constexpr uint32_t top_margin = 2u;

        auto                            base = detail::create_extended_layout(layout, top_margin, 3u, 3u);
        obstruction_layout<gate_layout> shifted_layout{base};
        detail::copy_layout_with_vertical_offset(layout, shifted_layout, top_margin);

        detail::assign_corridor_clock_numbers(shifted_layout, static_cast<uint32_t>(layout.x()),
                                              static_cast<uint32_t>(layout.y()), top_margin);

        const auto expected_clock = [&shifted_layout](const auto& t)
        {
            return static_cast<typename decltype(shifted_layout)::clock_number_t>(
                (t.x + t.y) % static_cast<uint64_t>(shifted_layout.num_clocks()));
        };

        const tile<decltype(shifted_layout)> top_corridor_tile{0, 0};
        const tile<decltype(shifted_layout)> bottom_corridor_tile{0,
                                                                  top_margin + static_cast<uint32_t>(layout.y()) + 1u};
        const tile<decltype(shifted_layout)> right_corridor_tile{static_cast<uint32_t>(layout.x()) + 1u, top_margin};

        CHECK(shifted_layout.get_clock_number(top_corridor_tile) == expected_clock(top_corridor_tile));
        CHECK(shifted_layout.get_clock_number(bottom_corridor_tile) == expected_clock(bottom_corridor_tile));
        CHECK(shifted_layout.get_clock_number(right_corridor_tile) == expected_clock(right_corridor_tile));
    }

    SECTION("Explicit pin routing order")
    {
        const auto ordered_pairs = detail::create_ordered_return_route_pairs(layout, 2u, {1u, 0u});

        REQUIRE(ordered_pairs.size() == 2u);
        CHECK(ordered_pairs[0].index == 1u);
        CHECK(ordered_pairs[1].index == 0u);
        CHECK(ordered_pairs[0].lane == 0u);
        CHECK(ordered_pairs[1].lane == 1u);
    }

    SECTION("Whitelist keeps occupied tiles obstructed")
    {
        constexpr uint32_t top_margin = 2u;

        auto                            base = detail::create_extended_layout(layout, top_margin, 3u, 3u);
        obstruction_layout<gate_layout> shifted_layout{base};
        detail::copy_layout_with_vertical_offset(layout, shifted_layout, top_margin);

        const auto route_pairs = detail::create_ordered_return_route_pairs(shifted_layout, 2u, {});

        std::vector<std::vector<tile<decltype(shifted_layout)>>> route_waypoints{};
        route_waypoints.reserve(route_pairs.size());
        for (const auto& pair : route_pairs)
        {
            route_waypoints.push_back(detail::create_return_route_waypoints(
                pair, static_cast<uint32_t>(layout.x()), static_cast<uint32_t>(layout.y()), top_margin, 2u));
        }

        const auto occupied_tile = shifted_layout.get_tile(shifted_layout.pi_at(0u));
        const std::vector<route_return_path_coordinate> whitelist{{occupied_tile.x, occupied_tile.y, occupied_tile.z}};

        detail::apply_routing_whitelist_obstructions(shifted_layout, whitelist, route_pairs, route_waypoints);

        CHECK(shifted_layout.is_obstructed_coordinate(occupied_tile));

        tile<decltype(shifted_layout)> empty_non_whitelisted{};
        bool                           found_empty_non_whitelisted = false;
        for (uint64_t y = 0u; y <= shifted_layout.y() && !found_empty_non_whitelisted; ++y)
        {
            for (uint64_t x = 0u; x <= shifted_layout.x(); ++x)
            {
                const tile<decltype(shifted_layout)> t{x, y, 0u};
                if (shifted_layout.is_empty_tile(t) && !(t == occupied_tile))
                {
                    empty_non_whitelisted       = t;
                    found_empty_non_whitelisted = true;
                    break;
                }
            }
        }
        REQUIRE(found_empty_non_whitelisted);
        CHECK(shifted_layout.is_obstructed_coordinate(empty_non_whitelisted));
    }
}

TEST_CASE("Route return path routing", "[route-return-path]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    const auto layout = create_two_pi_two_po_layout<gate_layout>();

    route_return_path_params params{};
    params.top_margin    = 0u;
    params.bottom_margin = 0u;
    params.right_margin  = 0u;
    params.lane_spacing  = 2u;

    route_return_path_stats stats{};
    const auto              routed_layout = route_return_path(layout, params, &stats);

    CHECK(routed_layout.num_pis() == layout.num_pis());
    CHECK(routed_layout.num_pos() == layout.num_pos());
    CHECK(routed_layout.num_wires() > layout.num_wires());
    CHECK(routed_layout.x() > layout.x());
    CHECK(routed_layout.y() > layout.y());

    CHECK(stats.num_routed_pairs == 2u);
    CHECK(stats.num_routed_segments >= stats.num_routed_pairs);

    routed_layout.foreach_pi(
        [&routed_layout](const auto& pi)
        {
            const auto pi_tile = routed_layout.get_tile(pi);
            CHECK_FALSE(routed_layout.has_no_incoming_signal<false>(pi_tile));
            CHECK(routed_layout.fanin_size<false>(pi) > 0u);
        });

    CHECK(routed_layout.num_crossings() == 0u);
}

TEST_CASE("Route return path routing with single-pin whitelist", "[route-return-path]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    const auto layout = create_single_pi_single_po_layout<gate_layout>();

    route_return_path_params params{};
    params.top_margin        = 0u;
    params.bottom_margin     = 0u;
    params.right_margin      = 0u;
    params.lane_spacing      = 1u;
    params.pin_routing_order = {0u};
    params.routing_whitelist = build_whitelist_from_planned_routes(layout, params);

    route_return_path_stats stats{};
    const auto              routed_layout = route_return_path(layout, params, &stats);

    CHECK(stats.num_routed_pairs == 1u);
    CHECK(stats.num_routed_segments >= 1u);
    CHECK(routed_layout.num_crossings() == 0u);

    routed_layout.foreach_pi(
        [&routed_layout](const auto& pi)
        {
            const auto pi_tile = routed_layout.get_tile(pi);
            CHECK_FALSE(routed_layout.has_no_incoming_signal<false>(pi_tile));
            CHECK(routed_layout.fanin_size<false>(pi) > 0u);
        });

    const auto whitelist_ids = whitelist_as_tile_ids<gate_layout>(params.routing_whitelist);

    routed_layout.foreach_wire(
        [&routed_layout, &whitelist_ids](const auto& wire)
        {
            const auto wire_tile = routed_layout.get_tile(wire);
            CHECK(whitelist_ids.count(static_cast<uint64_t>(wire_tile)) > 0u);
        });
}

TEST_CASE("Route return path routing with multi-pin U-shape whitelist and custom order", "[route-return-path]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    const auto layout = create_five_pi_five_po_layout<gate_layout>();

    route_return_path_params params{};
    params.top_margin        = 0u;
    params.bottom_margin     = 0u;
    params.right_margin      = 0u;
    params.lane_spacing      = 1u;
    params.pin_routing_order = {4u, 3u, 2u, 1u, 0u};
    params.routing_whitelist = build_whitelist_from_planned_routes(layout, params);

    route_return_path_stats stats{};
    const auto              routed_layout = route_return_path(layout, params, &stats);

    CHECK(stats.num_routed_pairs == 5u);
    CHECK(stats.num_routed_segments >= stats.num_routed_pairs);
    CHECK(routed_layout.num_crossings() == 0u);

    routed_layout.foreach_pi(
        [&routed_layout](const auto& pi)
        {
            const auto pi_tile = routed_layout.get_tile(pi);
            CHECK_FALSE(routed_layout.has_no_incoming_signal<false>(pi_tile));
            CHECK(routed_layout.fanin_size<false>(pi) > 0u);
        });

    bool has_right_corridor_wire = false;
    routed_layout.foreach_wire(
        [&routed_layout, &has_right_corridor_wire, &layout](const auto& wire)
        {
            if (routed_layout.get_tile(wire).x > layout.x())
            {
                has_right_corridor_wire = true;
            }
        });
    CHECK(has_right_corridor_wire);
}

TEST_CASE("Route return path final-segment clocking policy", "[route-return-path]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    const auto layout = create_single_pi_single_po_u_layout<gate_layout>();

    route_return_path_params permissive_params{};
    permissive_params.top_margin                           = 2u;
    permissive_params.bottom_margin                        = 0u;
    permissive_params.right_margin                         = 0u;
    permissive_params.lane_spacing                         = 1u;
    permissive_params.pin_routing_order                    = {0u};
    permissive_params.allow_invalid_final_segment_clocking = true;

    route_return_path_stats permissive_stats{};
    const auto              permissive_layout = route_return_path(layout, permissive_params, &permissive_stats);

    route_return_path_params strict_params             = permissive_params;
    strict_params.allow_invalid_final_segment_clocking = false;

    route_return_path_stats strict_stats{};
    const auto              strict_layout = route_return_path(layout, strict_params, &strict_stats);

    const auto permissive_pi = permissive_layout.pi_at(0u);
    const auto strict_pi     = strict_layout.pi_at(0u);

    CHECK(permissive_layout.fanin_size<false>(permissive_pi) > 0u);
    CHECK(permissive_layout.fanin_size(permissive_pi) == 0u);

    CHECK(strict_layout.fanin_size<false>(strict_pi) > 0u);
    CHECK(strict_layout.fanin_size(strict_pi) > 0u);

    CHECK(strict_layout.num_wires() >= permissive_layout.num_wires());
    CHECK(permissive_stats.num_routed_pairs == 1u);
    CHECK(strict_stats.num_routed_pairs == 1u);
}
