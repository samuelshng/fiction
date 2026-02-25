//
// Created by marcel on 23.02.26.
//

#include <catch2/catch_test_macros.hpp>

#include <fiction/algorithms/physical_design/route_return_path.hpp>
#include <fiction/io/dot_drawers.hpp>
#include <fiction/io/write_dot_return_path_overlay_layout.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>

#include <sstream>
#include <stdexcept>

using namespace fiction;

namespace
{

using gate_layout =
    gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

/**
 * @brief Creates a single-PI/single-PO layout with a U-shaped return target.
 *
 * @return Test layout.
 */
[[nodiscard]] gate_layout create_single_pi_single_po_u_layout()
{
    gate_layout lyt{{8, 4, 1}, row_clocking<gate_layout>()};

    const auto a = lyt.create_pi("a", {1, 0});
    lyt.create_po(a, "f", {6, 3});

    return lyt;
}

}  // namespace

TEST_CASE("Return-path overlay detects relaxed final segment", "[write-dot-return-path-overlay-layout]")
{
    const auto layout = create_single_pi_single_po_u_layout();

    route_return_path_params params{};
    params.top_margin                           = 2u;
    params.lane_spacing                         = 1u;
    params.pin_routing_order                    = {0u};
    params.allow_invalid_final_segment_clocking = true;

    const auto routed_layout    = route_return_path(layout, params);
    const auto reference_layout = create_return_path_reference_layout(layout, params);

    const auto overlay_relaxed = determine_return_path_overlay(reference_layout, routed_layout, false);
    const auto overlay_strict  = determine_return_path_overlay(reference_layout, routed_layout, true);

    const auto pi      = routed_layout.pi_at(0u);
    const auto pi_tile = routed_layout.get_tile(pi);

    tile<gate_layout> incoming_any_tile{};
    bool              found_incoming_any = false;

    const auto collect_any_fanin = [&incoming_any_tile, &found_incoming_any](const auto& fanin_signal)
    {
        if (!found_incoming_any)
        {
            incoming_any_tile  = static_cast<tile<gate_layout>>(fanin_signal);
            found_incoming_any = true;
        }
    };

    routed_layout.template foreach_fanin<decltype(collect_any_fanin), false>(pi, std::move(collect_any_fanin));

    REQUIRE(found_incoming_any);

    const return_path_overlay_edge incoming_any_edge{static_cast<uint64_t>(incoming_any_tile),
                                                     static_cast<uint64_t>(pi_tile)};

    CHECK(overlay_relaxed.overlay_tiles.count(static_cast<uint64_t>(incoming_any_tile)) > 0u);
    CHECK(overlay_relaxed.overlay_edges.count(incoming_any_edge) > 0u);
    CHECK(overlay_strict.overlay_edges.count(incoming_any_edge) == 0u);

    return_path_overlay_drawer_params drawer_params{};
    drawer_params.respect_clocking = false;

    std::stringstream dot_stream{};
    write_dot_return_path_overlay_layout(reference_layout, routed_layout, dot_stream, drawer_params);

    const gate_layout_hexagonal_drawer<gate_layout, false, false> drawer{};
    const auto edge_pattern = fmt::format("{} -> {}", drawer.tile_id(incoming_any_tile), drawer.tile_id(pi_tile));

    CHECK(dot_stream.str().find(edge_pattern) != std::string::npos);
    CHECK(dot_stream.str().find("firebrick") != std::string::npos);
}

TEST_CASE("Return-path overlay detects strict final segment", "[write-dot-return-path-overlay-layout]")
{
    const auto layout = create_single_pi_single_po_u_layout();

    route_return_path_params params{};
    params.top_margin                           = 2u;
    params.lane_spacing                         = 1u;
    params.pin_routing_order                    = {0u};
    params.allow_invalid_final_segment_clocking = false;

    const auto routed_layout    = route_return_path(layout, params);
    const auto reference_layout = create_return_path_reference_layout(layout, params);
    const auto overlay_strict   = determine_return_path_overlay(reference_layout, routed_layout, true);

    const auto pi      = routed_layout.pi_at(0u);
    const auto pi_tile = routed_layout.get_tile(pi);

    REQUIRE(routed_layout.fanin_size(pi) > 0u);

    tile<gate_layout> incoming_valid_tile{};
    bool              found_incoming_valid = false;

    const auto collect_valid_fanin = [&incoming_valid_tile, &found_incoming_valid](const auto& fanin_signal)
    {
        if (!found_incoming_valid)
        {
            incoming_valid_tile  = static_cast<tile<gate_layout>>(fanin_signal);
            found_incoming_valid = true;
        }
    };

    routed_layout.foreach_fanin(pi, collect_valid_fanin);

    REQUIRE(found_incoming_valid);

    const return_path_overlay_edge incoming_valid_edge{static_cast<uint64_t>(incoming_valid_tile),
                                                       static_cast<uint64_t>(pi_tile)};

    CHECK(overlay_strict.overlay_edges.count(incoming_valid_edge) > 0u);

    return_path_overlay_drawer_params drawer_params{};
    drawer_params.respect_clocking = true;

    std::stringstream dot_stream{};
    write_dot_return_path_overlay_layout(reference_layout, routed_layout, dot_stream, drawer_params);

    const gate_layout_hexagonal_drawer<gate_layout, false, false> drawer{};
    const auto edge_pattern = fmt::format("{} -> {}", drawer.tile_id(incoming_valid_tile), drawer.tile_id(pi_tile));

    CHECK(dot_stream.str().find(edge_pattern) != std::string::npos);
}

TEST_CASE("Return-path overlay rejects mismatching dimensions", "[write-dot-return-path-overlay-layout]")
{
    const auto layout = create_single_pi_single_po_u_layout();

    route_return_path_params params{};
    params.top_margin                           = 2u;
    params.lane_spacing                         = 1u;
    params.allow_invalid_final_segment_clocking = true;

    const auto routed_layout = route_return_path(layout, params);

    CHECK_THROWS_AS(determine_return_path_overlay(layout, routed_layout, false), std::invalid_argument);
}
