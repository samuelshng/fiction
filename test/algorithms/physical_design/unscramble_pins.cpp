//
// Created by marcel on 10.02.26.
//

#include <catch2/catch_test_macros.hpp>

#include <fiction/algorithms/physical_design/unscramble_pins.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>

using namespace fiction;

TEST_CASE("Helper functions for unscramble_pins", "[unscramble-pins]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    gate_layout layout{gate_layout::aspect_ratio{10, 10, 0}, row_clocking<gate_layout>()};

    const auto x1 = layout.create_pi("x1", {0, 0});  // Slot 0
    const auto x2 = layout.create_pi("x2", {2, 0});  // Slot 1
    const auto x3 = layout.create_pi("x3", {4, 0});  // Slot 2

    const std::vector current_pis{layout.get_node(x1), layout.get_node(x2), layout.get_node(x3)};

    SECTION("determine_pin_coordinates")
    {
        const auto coords = detail::determine_pin_coordinates(layout, current_pis);
        REQUIRE(coords.size() == 3);
        CHECK(coords[0] == tile<gate_layout>{0, 0});
        CHECK(coords[1] == tile<gate_layout>{2, 0});
        CHECK(coords[2] == tile<gate_layout>{4, 0});
    }

    SECTION("calculate_rows_needed")
    {
        SECTION("Identity")
        {
            // x1->x1 (0->0), x2->x2 (2->2), x3->x3 (4->4)
            // Distance 0.
            const auto rows = detail::calculate_rows_needed(layout, current_pis, current_pis);
            CHECK(rows == 0);
        }

        SECTION("Swap x1 and x2")
        {
            // Target: {x2, x1, x3}
            // Slot 0 (x=0) wants x2. x2 is at x=2. Dist=2. Rows=4.
            // Slot 1 (x=2) wants x1. x1 is at x=0. Dist=2. Rows=4.
            // Slot 2 (x=4) wants x3. x3 is at x=4. Dist=0. Rows=0.
            // Max rows = 4.
            const std::vector target{layout.get_node(x2), layout.get_node(x1), layout.get_node(x3)};
            const auto        rows = detail::calculate_rows_needed(layout, current_pis, target);
            CHECK(rows == 4);
        }

        SECTION("Rotate x1, x2, x3 -> x2, x3, x1")
        {
            // Target: {x2, x3, x1}
            // Slot 0 (x=0) wants x2 (at x=2). Dist=2. Rows=4.
            // Slot 1 (x=2) wants x3 (at x=4). Dist=2. Rows=4.
            // Slot 2 (x=4) wants x1 (at x=0). Dist=4. Rows=8.
            // Max rows = 8.
            const std::vector target{layout.get_node(x2), layout.get_node(x3), layout.get_node(x1)};
            const auto        rows = detail::calculate_rows_needed(layout, current_pis, target);
            CHECK(rows == 8);
        }

        SECTION("Reverse x1, x2, x3 -> x3, x2, x1")
        {
            // Target: {x3, x2, x1}
            // Slot 0 (x=0) wants x3 (at x=4). Dist=4. Rows=8.
            // Slot 1 (x=2) wants x2 (at x=2). Dist=0.
            // Slot 2 (x=4) wants x1 (at x=0). Dist=4. Rows=8.
            // Max rows = 8.
            const std::vector target{layout.get_node(x3), layout.get_node(x2), layout.get_node(x1)};
            const auto        rows = detail::calculate_rows_needed(layout, current_pis, target);
            CHECK(rows == 8);
        }
    }
}
