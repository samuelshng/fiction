//
// Created by marcel on 10.02.26.
//

#include <catch2/catch_test_macros.hpp>

#include <fiction/algorithms/physical_design/unscramble_pins.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>

#include <numeric>
#include <vector>

using namespace fiction;

TEST_CASE("Unscramble pins helper functions", "[unscramble-pins]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    gate_layout layout{{20, 20}, row_clocking<gate_layout>()};

    // PI setup
    const auto x1 = layout.create_pi("x1", {0, 0});  // Slot 0
    const auto x2 = layout.create_pi("x2", {2, 0});  // Slot 1
    const auto x3 = layout.create_pi("x3", {4, 0});  // Slot 2

    const std::vector current_pis{layout.get_node(x1), layout.get_node(x2), layout.get_node(x3)};

    // PO setup
    const auto po1 = layout.create_po(x1, "po1", {0, 10});  // Slot 0
    const auto po2 = layout.create_po(x2, "po2", {2, 10});  // Slot 1
    const auto po3 = layout.create_po(x3, "po3", {4, 10});  // Slot 2

    const std::vector current_pos{layout.get_node(po1), layout.get_node(po2), layout.get_node(po3)};

    SECTION("determine_pin_coordinates")
    {
        SECTION("Primary Inputs")
        {
            const auto coords = detail::determine_pin_coordinates(layout, current_pis);
            REQUIRE(coords.size() == 3);
            CHECK(coords[0] == tile<gate_layout>{0, 0});
            CHECK(coords[1] == tile<gate_layout>{2, 0});
            CHECK(coords[2] == tile<gate_layout>{4, 0});
        }

        SECTION("Primary Outputs")
        {
            const auto coords = detail::determine_pin_coordinates(layout, current_pos);
            REQUIRE(coords.size() == 3);
            CHECK(coords[0] == tile<gate_layout>{0, 10});
            CHECK(coords[1] == tile<gate_layout>{2, 10});
            CHECK(coords[2] == tile<gate_layout>{4, 10});
        }
    }

    SECTION("calculate_rows_needed")
    {
        SECTION("Primary Inputs")
        {
            SECTION("Identity")
            {
                CHECK(detail::calculate_rows_needed(layout, current_pis, current_pis) == 0);
            }

            SECTION("Small permutations")
            {
                SECTION("Swap")
                {
                    const std::vector target{layout.get_node(x2), layout.get_node(x1), layout.get_node(x3)};
                    CHECK(detail::calculate_rows_needed(layout, current_pis, target) == 4);
                }
                SECTION("Rotate")
                {
                    const std::vector target{layout.get_node(x2), layout.get_node(x3), layout.get_node(x1)};
                    CHECK(detail::calculate_rows_needed(layout, current_pis, target) == 8);
                }
                SECTION("Reverse")
                {
                    const std::vector target{layout.get_node(x3), layout.get_node(x2), layout.get_node(x1)};
                    CHECK(detail::calculate_rows_needed(layout, current_pis, target) == 8);
                }
            }

            SECTION("Larger permutations")
            {
                std::vector<mockturtle::node<gate_layout>> large_pis{};
                for (uint32_t i = 0; i < 10; ++i)
                {
                    large_pis.push_back(layout.get_node(layout.create_pi(std::to_string(i), {i * 2, 1})));
                }

                SECTION("Shift by one")
                {
                    std::vector<mockturtle::node<gate_layout>> target(large_pis.size());
                    target[0] = large_pis[9];
                    for (size_t i = 1; i < 10; ++i)
                    {
                        target[i] = large_pis[i - 1];
                    }

                    CHECK(detail::calculate_rows_needed(layout, large_pis, target) == 36);
                }

                SECTION("Full reverse")
                {
                    std::vector<mockturtle::node<gate_layout>> target = large_pis;
                    std::reverse(target.begin(), target.end());

                    CHECK(detail::calculate_rows_needed(layout, large_pis, target) == 36);
                }
            }
        }

        SECTION("Primary Outputs")
        {
            SECTION("Identity")
            {
                CHECK(detail::calculate_rows_needed(layout, current_pos, current_pos) == 0);
            }

            SECTION("Small permutations")
            {
                SECTION("Swap")
                {
                    const std::vector target{layout.get_node(po3), layout.get_node(po2), layout.get_node(po1)};
                    CHECK(detail::calculate_rows_needed(layout, current_pos, target) == 8);
                }
                SECTION("Rotate")
                {
                    const std::vector target{layout.get_node(po2), layout.get_node(po3), layout.get_node(po1)};
                    CHECK(detail::calculate_rows_needed(layout, current_pos, target) == 8);
                }
                SECTION("Reverse")
                {
                    const std::vector target{layout.get_node(po3), layout.get_node(po2), layout.get_node(po1)};
                    CHECK(detail::calculate_rows_needed(layout, current_pos, target) == 8);
                }
            }

            SECTION("Larger permutations")
            {
                std::vector<mockturtle::node<gate_layout>> large_pos{};
                for (uint32_t i = 0; i < 10; ++i)
                {
                    const auto pi = layout.create_pi(std::to_string(i), {i * 2, 11});
                    large_pos.push_back(layout.get_node(layout.create_po(pi, std::to_string(i), {i * 2, 12})));
                }

                SECTION("Shift by one")
                {
                    std::vector<mockturtle::node<gate_layout>> target(large_pos.size());
                    target[0] = large_pos[9];
                    for (size_t i = 1; i < 10; ++i)
                    {
                        target[i] = large_pos[i - 1];
                    }

                    CHECK(detail::calculate_rows_needed(layout, large_pos, target) == 36);
                }

                SECTION("Full reverse")
                {
                    std::vector<mockturtle::node<gate_layout>> target = large_pos;
                    std::reverse(target.begin(), target.end());

                    CHECK(detail::calculate_rows_needed(layout, large_pos, target) == 36);
                }
            }
        }

        SECTION("Corner cases")
        {
            SECTION("Empty permutations")
            {
                CHECK(detail::calculate_rows_needed(layout, {}, {}) == 0);
                CHECK(detail::calculate_rows_needed(layout, current_pis, {}) == 0);
                CHECK(detail::calculate_rows_needed(layout, {}, current_pis) == 0);
            }

            SECTION("Single pin")
            {
                const std::vector single_pi{layout.get_node(x1)};
                CHECK(detail::calculate_rows_needed(layout, single_pi, single_pi) == 0);
            }

            SECTION("Node not in current permutation")
            {
                const auto        x4 = layout.create_pi("x4", {6, 0});
                const std::vector target{layout.get_node(x4), layout.get_node(x2), layout.get_node(x3)};
                // x4 is not in current_pis, so it's skipped. Max rows from others (identity) is 0.
                CHECK(detail::calculate_rows_needed(layout, current_pis, target) == 0);
            }

            SECTION("Mismatched sizes")
            {
                // Desired is smaller than current
                const std::vector target_small{layout.get_node(x2), layout.get_node(x1)};
                // Slot 0 wants x2 (at x=2). Dist=2. Rows=4.
                // Slot 1 wants x1 (at x=0). Dist=2. Rows=4.
                CHECK(detail::calculate_rows_needed(layout, current_pis, target_small) == 4);
            }
        }
    }
}
