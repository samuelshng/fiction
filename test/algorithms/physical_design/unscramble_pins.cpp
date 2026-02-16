//
// Created by marcel on 10.02.26.
//

#include <catch2/catch_test_macros.hpp>

#include "utils/equivalence_checking_utils.hpp"

#include <fiction/algorithms/physical_design/unscramble_pins.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>

#include <algorithm>
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

    SECTION("calculate_permutation_distances")
    {
        SECTION("Identity")
        {
            const auto distances = detail::calculate_permutation_distances(layout, current_pis, current_pis);
            REQUIRE(distances.size() == 3);
            CHECK(distances[0] == 0);
            CHECK(distances[1] == 0);
            CHECK(distances[2] == 0);
        }

        SECTION("Swap")
        {
            const std::vector target{layout.get_node(x2), layout.get_node(x1), layout.get_node(x3)};
            const auto        distances = detail::calculate_permutation_distances(layout, current_pis, target);
            REQUIRE(distances.size() == 3);
            CHECK(distances[0] == 2);
            CHECK(distances[1] == 2);
            CHECK(distances[2] == 0);
        }

        SECTION("Empty desired")
        {
            const auto distances = detail::calculate_permutation_distances(layout, current_pis, {});
            CHECK(distances.empty());
        }

        SECTION("Missing node in current permutation")
        {
            const auto        x4 = layout.create_pi("x4", {6, 0});
            const std::vector target{layout.get_node(x4), layout.get_node(x2), layout.get_node(x3)};
            const auto        distances = detail::calculate_permutation_distances(layout, current_pis, target);
            REQUIRE(distances.size() == 3);
            CHECK(distances[0] == 0);
            CHECK(distances[1] == 0);
            CHECK(distances[2] == 0);
        }

        SECTION("Desired shorter than current")
        {
            const std::vector target{layout.get_node(x2), layout.get_node(x1)};
            const auto        distances = detail::calculate_permutation_distances(layout, current_pis, target);
            REQUIRE(distances.size() == 2);
            CHECK(distances[0] == 2);
            CHECK(distances[1] == 2);
        }
    }

    SECTION("create_extended_layout")
    {
        const auto new_layout = detail::create_extended_layout(layout, 4, 6);
        CHECK(new_layout.x() == layout.x());
        CHECK(new_layout.y() == layout.y() + 10);
        CHECK(new_layout.z() == layout.z());
    }

    SECTION("copy_layout_with_offset")
    {
        SECTION("Empty layout")
        {
            const gate_layout empty_layout{{4, 4}, row_clocking<gate_layout>()};
            auto              target = detail::create_extended_layout(empty_layout, 2, 0);

            detail::copy_layout_with_offset(empty_layout, target, 2);

            CHECK(target.num_pis() == 0);
            CHECK(target.num_gates() == 0);
            CHECK(target.num_wires() == 0);
            CHECK(target.num_pos() == 0);
        }

        SECTION("PI only")
        {
            gate_layout pi_layout{{4, 4}, row_clocking<gate_layout>()};
            pi_layout.create_pi("pi1", {0, 0});
            pi_layout.create_pi("pi2", {2, 0});

            auto target = detail::create_extended_layout(pi_layout, 3, 0);
            detail::copy_layout_with_offset(pi_layout, target, 3);

            CHECK(target.num_pis() == 2);
            CHECK(target.get_tile(target.pi_at(0)) == tile<gate_layout>{0, 3});
            CHECK(target.get_tile(target.pi_at(1)) == tile<gate_layout>{2, 3});
            CHECK(target.get_name(target.pi_at(0)) == "pi1");
            CHECK(target.get_name(target.pi_at(1)) == "pi2");
        }

        SECTION("Simple gate")
        {
            gate_layout simple{{4, 4}, row_clocking<gate_layout>()};
            const auto  pi1      = simple.create_pi("x1", {0, 0});
            const auto  pi2      = simple.create_pi("x2", {2, 0});
            const auto  and_gate = simple.create_and(pi1, pi2, {1, 1});
            simple.create_po(and_gate, "f1", {1, 2});

            auto target = detail::create_extended_layout(simple, 2, 0);
            detail::copy_layout_with_offset(simple, target, 2);

            CHECK(target.num_pis() == 2);
            CHECK(target.num_gates() == 1);
            CHECK(target.num_pos() == 1);

            // Check coordinates shifted by 2
            CHECK(target.get_tile(target.pi_at(0)) == tile<gate_layout>{0, 2});
            CHECK(target.get_tile(target.pi_at(1)) == tile<gate_layout>{2, 2});
            CHECK(target.get_tile(target.get_node(target.po_at(0))) == tile<gate_layout>{1, 4});

            // Check names preserved
            CHECK(target.get_name(target.pi_at(0)) == "x1");
            CHECK(target.get_name(target.pi_at(1)) == "x2");
            CHECK(target.get_output_name(0) == "f1");
        }

        SECTION("Complex layout with buffers")
        {
            gate_layout complex{{6, 6}, row_clocking<gate_layout>()};
            const auto  pi1 = complex.create_pi("in1", {0, 0});
            const auto  pi2 = complex.create_pi("in2", {2, 0});

            const auto buf1 = complex.create_buf(pi1, {0, 1});
            const auto buf2 = complex.create_buf(pi2, {2, 1});

            const auto or_gate = complex.create_or(buf1, buf2, {1, 2});
            complex.create_po(or_gate, "out1", {1, 3});

            auto target = detail::create_extended_layout(complex, 4, 0);
            detail::copy_layout_with_offset(complex, target, 4);

            CHECK(target.num_pis() == complex.num_pis());
            CHECK(target.num_wires() == complex.num_wires());
            CHECK(target.num_gates() == complex.num_gates());
            CHECK(target.num_pos() == complex.num_pos());

            // Check all coordinates shifted by 4
            CHECK(target.get_tile(target.pi_at(0)) == tile<gate_layout>{0, 4});
            CHECK(target.get_tile(target.pi_at(1)) == tile<gate_layout>{2, 4});
            CHECK(target.get_tile(target.get_node(target.po_at(0))) == tile<gate_layout>{1, 7});
        }

        SECTION("Layout with multiple gates")
        {
            gate_layout multi{{6, 6}, row_clocking<gate_layout>()};
            const auto  pi1 = multi.create_pi("a", {0, 0});
            const auto  pi2 = multi.create_pi("b", {2, 0});
            const auto  pi3 = multi.create_pi("c", {4, 0});

            const auto and1 = multi.create_and(pi1, pi2, {0, 1});
            const auto or1  = multi.create_or(pi2, pi3, {2, 1});

            const auto xor1 = multi.create_xor(and1, or1, {1, 2});
            multi.create_po(xor1, "result", {1, 3});

            auto target = detail::create_extended_layout(multi, 5, 0);
            detail::copy_layout_with_offset(multi, target, 5);

            CHECK(target.num_pis() == 3);
            CHECK(target.num_gates() == 3);
            CHECK(target.num_pos() == 1);

            // Verify y-coordinates are all shifted by 5
            CHECK(target.get_tile(target.pi_at(0)).y == 5);
            CHECK(target.get_tile(target.pi_at(1)).y == 5);
            CHECK(target.get_tile(target.pi_at(2)).y == 5);
            CHECK(target.get_tile(target.get_node(target.po_at(0))).y == 8);
        }

        SECTION("Layout with inverted signals")
        {
            gate_layout inv_layout{{4, 4}, row_clocking<gate_layout>()};
            const auto  pi1  = inv_layout.create_pi("x", {0, 0});
            const auto  inv1 = inv_layout.create_not(pi1, {0, 1});
            inv_layout.create_po(inv1, "not_x", {0, 2});

            auto target = detail::create_extended_layout(inv_layout, 3, 0);
            detail::copy_layout_with_offset(inv_layout, target, 3);

            CHECK(target.num_pis() == 1);
            CHECK(target.num_gates() == 1);
            CHECK(target.num_pos() == 1);

            // Check the NOT gate exists
            bool found_not_gate = false;
            target.foreach_gate(
                [&](const auto& gate)
                {
                    if (target.is_inv(gate))
                    {
                        found_not_gate = true;
                    }
                });
            CHECK(found_not_gate);
        }

        SECTION("Zero offset")
        {
            gate_layout zero_offset{{4, 4}, row_clocking<gate_layout>()};
            const auto  pi1  = zero_offset.create_pi("x", {0, 0});
            const auto  buf1 = zero_offset.create_buf(pi1, {0, 1});
            zero_offset.create_po(buf1, "f", {0, 2});

            auto target = detail::create_extended_layout(zero_offset, 0, 0);
            detail::copy_layout_with_offset(zero_offset, target, 0);

            CHECK(target.num_pis() == zero_offset.num_pis());
            CHECK(target.num_wires() == zero_offset.num_wires());
            CHECK(target.num_pos() == zero_offset.num_pos());

            // Check coordinates are the same
            CHECK(target.get_tile(target.pi_at(0)) == tile<gate_layout>{0, 0});
            CHECK(target.get_tile(target.get_node(target.po_at(0))) == tile<gate_layout>{0, 2});
        }
    }

    SECTION("create_pi_routing_objectives")
    {
        const std::vector desired{layout.get_node(x2), layout.get_node(x1), layout.get_node(x3)};
        auto              new_layout = detail::create_extended_layout(layout, 4, 0);

        const auto objectives = detail::create_pi_routing_objectives(layout, new_layout, current_pis, desired, 4);

        REQUIRE(objectives.size() == 3);
        CHECK(new_layout.num_pis() == 3);

        const auto distance = [](const auto& obj)
        {
            return static_cast<uint32_t>(
                std::abs(static_cast<int32_t>(obj.source.x) - static_cast<int32_t>(obj.target.x)));
        };

        CHECK(distance(objectives[0]) >= distance(objectives[1]));
        CHECK(distance(objectives[1]) >= distance(objectives[2]));

        SECTION("Targets shifted by pi_rows")
        {
            for (const auto& [source, target] : objectives)
            {
                CHECK(target.y >= 4);
            }
        }
    }

    SECTION("create_pi_routing_objectives empty desired")
    {
        auto       new_layout = detail::create_extended_layout(layout, 2, 0);
        const auto objectives = detail::create_pi_routing_objectives(layout, new_layout, current_pis, {}, 2);
        CHECK(objectives.empty());
        CHECK(new_layout.num_pis() == 0);
    }

    SECTION("create_pi_routing_objectives desired shorter")
    {
        const std::vector desired{layout.get_node(x2)};
        auto              new_layout = detail::create_extended_layout(layout, 3, 0);

        const auto objectives = detail::create_pi_routing_objectives(layout, new_layout, current_pis, desired, 3);

        REQUIRE(objectives.size() == 1);
        CHECK(new_layout.num_pis() == 1);
        CHECK(objectives[0].source.y == 0);
        CHECK(objectives[0].target.y >= 3);
    }

    SECTION("route_objectives_with_a_star")
    {
        gate_layout routing_layout{{6, 6}, row_clocking<gate_layout>()};

        const auto a = routing_layout.create_pi("a", {0, 0});
        const auto b = routing_layout.create_pi("b", {2, 0});

        const auto route_po1 = routing_layout.create_po(a, "po1", {0, 4});
        const auto route_po2 = routing_layout.create_po(b, "po2", {2, 4});

        const auto before_wires = routing_layout.num_wires();

        const std::vector objectives{
            routing_objective<gate_layout>{{0, 0}, {0, 4}},
            routing_objective<gate_layout>{{2, 0}, {2, 4}},
        };

        detail::route_objectives_with_a_star(routing_layout, objectives);

        CHECK(routing_layout.num_wires() > before_wires);
        CHECK(routing_layout.get_node(tile<gate_layout>{0, 4}) == routing_layout.get_node(route_po1));
        CHECK(routing_layout.get_node(tile<gate_layout>{2, 4}) == routing_layout.get_node(route_po2));
    }
}

TEST_CASE("Unscramble pins equivalence checking", "[unscramble-pins]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

    SECTION("2-input AND gate")
    {
        gate_layout layout{{4, 4, 1}, row_clocking<gate_layout>()};

        const auto x1 = layout.create_pi("x1", {1, 0});
        const auto x2 = layout.create_pi("x2", {2, 0});
        const auto a1 = layout.create_and(x1, x2, {1, 1});
        layout.create_po(a1, "f1", {1, 2});

        const std::vector pis{layout.get_node(x1), layout.get_node(x2)};

        SECTION("PI permutations")
        {
            auto target_pis = pis;
            std::sort(target_pis.begin(), target_pis.end());

            do
            {
                check_eq(layout, unscramble_pins(layout, target_pis, {}));
            } while (std::next_permutation(target_pis.begin(), target_pis.end()));
        }
    }

    SECTION("Multiple outputs")
    {
        gate_layout layout{{4, 4, 1}, row_clocking<gate_layout>()};

        const auto x1 = layout.create_pi("x1", {0, 0});
        const auto x2 = layout.create_pi("x2", {1, 0});
        const auto x3 = layout.create_pi("x3", {2, 0});

        const auto buf1 = layout.create_buf(x1, {0, 1});
        const auto and1 = layout.create_and(x2, x3, {1, 1});

        const auto buf2 = layout.create_buf(buf1, {1, 2});
        const auto fo1  = layout.create_buf(and1, {2, 2});

        const auto or1  = layout.create_or(buf2, fo1, {1, 3});
        const auto buf3 = layout.create_buf(fo1, {2, 3});

        const auto po1 = layout.create_po(or1, "po1", {1, 4});
        const auto po2 = layout.create_po(buf3, "po2", {3, 4});

        const std::vector pos{layout.get_node(po1), layout.get_node(po2)};

        SECTION("PO permutations")
        {
            auto target_pos = pos;
            std::sort(target_pos.begin(), target_pos.end());

            do
            {
                check_eq(layout, unscramble_pins(layout, {}, target_pos));
            } while (std::next_permutation(target_pos.begin(), target_pos.end()));
        }
    }
}
