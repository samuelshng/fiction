/**
 * @file post_layout_optimization_hex.cpp
 * @brief Tests for native post-layout optimization on row-clocked hexagonal layouts.
 */

#include <catch2/catch_test_macros.hpp>

#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"

#include <fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp>
#include <fiction/algorithms/physical_design/orthogonal_hex.hpp>
#include <fiction/algorithms/physical_design/post_layout_optimization_hex.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/technology_network.hpp>

#include <cstdint>

using namespace fiction;

namespace
{

template <typename Lyt, typename Ntk, typename LayoutCreator>
void check_hex_optimization(const Ntk& ntk, const LayoutCreator& create_layout)
{
    auto layout = create_layout(ntk);
    layout.resize({layout.x() + 2u, layout.y() + 2u, layout.z()});

    const auto x_before    = layout.x() + 1u;
    const auto y_before    = layout.y() + 1u;
    const auto area_before = static_cast<uint64_t>(layout.x() + 1u) * static_cast<uint64_t>(layout.y() + 1u);

    post_layout_optimization_stats stats{};
    post_layout_optimization_hex(layout, {}, &stats);

    const auto area_after = static_cast<uint64_t>(layout.x() + 1u) * static_cast<uint64_t>(layout.y() + 1u);

    CHECK(layout.is_clocking_scheme(clock_name::ROW));
    CHECK(area_after <= area_before);
    CHECK(stats.x_size_before == x_before);
    CHECK(stats.y_size_before == y_before);
    check_eq(ntk, layout);

    layout.foreach_pi(
        [&layout](const auto& pi)
        {
            CHECK(layout.get_tile(pi).y == 0u);
        });

    layout.foreach_po(
        [&layout](const auto& po)
        {
            CHECK(layout.get_tile(layout.get_node(po)).y == layout.y());
        });
}

}  // namespace

TEST_CASE("Native hexagonal post-layout optimization preserves orthogonal hex layouts",
          "[post_layout_optimization][post_layout_optimization_hex]")
{
    using odd_row_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;
    using even_row_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

    SECTION("odd row")
    {
        check_hex_optimization<odd_row_layout>(
            blueprints::full_adder_network<technology_network>(),
            [](const auto& ntk) { return orthogonal_hex<odd_row_layout>(ntk); });
    }

    SECTION("even row")
    {
        check_hex_optimization<even_row_layout>(
            blueprints::full_adder_network<technology_network>(),
            [](const auto& ntk) { return orthogonal_hex<even_row_layout>(ntk); });
    }
}

TEST_CASE("Native hexagonal post-layout optimization preserves hex GOLD layouts",
          "[post_layout_optimization][post_layout_optimization_hex]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

    const auto ntk = blueprints::and_or_network<technology_network>();

    graph_oriented_layout_design_params params{};
    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
    params.return_first = true;
    params.seed         = 0u;
    params.timeout      = 10000u;

    auto layout = graph_oriented_layout_design_hex<gate_layout>(ntk, params);
    REQUIRE(layout.has_value());
    check_eq(ntk, *layout);

    const auto area_before = (layout->x() + 1u) * (layout->y() + 1u);

    post_layout_optimization_stats stats{};
    post_layout_optimization_hex(*layout, {}, &stats);

    const auto area_after = (layout->x() + 1u) * (layout->y() + 1u);

    CHECK(layout->is_clocking_scheme(clock_name::ROW));
    CHECK(area_after <= area_before);
    check_eq(ntk, *layout);
}
