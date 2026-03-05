//
// Created by marcel on 10.02.26.
//

#include <catch2/catch_test_macros.hpp>

#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"

#include <fiction/algorithms/physical_design/graph_oriented_layout_design.hpp>
#include <fiction/algorithms/physical_design/hexagonalization.hpp>
#include <fiction/algorithms/physical_design/unscramble_pins.hpp>
#include <fiction/layouts/cartesian_layout.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/technology_network.hpp>
#include <fiction/types.hpp>
#include <fiction/utils/debug/network_writer.hpp>

#include <algorithm>
#include <filesystem>
#include <vector>

using namespace fiction;

namespace
{

template <typename Ntk>
hex_even_row_gate_clk_lyt generate_extended_hex_layout_from_network(const Ntk& ntk)
{
    using cart_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

    graph_oriented_layout_design_params gold_params{};
    gold_params.mode                  = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
    gold_params.cost                  = graph_oriented_layout_design_params::cost_objective::WIRES;
    gold_params.seed                  = 0u;
    gold_params.return_first          = true;
    gold_params.enable_multithreading = true;
    gold_params.timeout               = 10000u;

    const auto cart_layout_opt = graph_oriented_layout_design<cart_layout>(ntk, gold_params);
    REQUIRE(cart_layout_opt.has_value());

    hexagonalization_params hex_params{};
    hex_params.input_pin_extension  = hexagonalization_params::io_pin_extension_mode::EXTEND;
    hex_params.output_pin_extension = hexagonalization_params::io_pin_extension_mode::EXTEND;

    return hexagonalization<hex_even_row_gate_clk_lyt, cart_layout>(*cart_layout_opt, hex_params);
}

}  // namespace

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
                large_pis.reserve(10);
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
        CHECK(new_layout.z() == std::max(layout.z(), static_cast<decltype(layout.z())>(1)));
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

            CHECK(target.num_pis() == 0);
            CHECK(target.num_pos() == 0);
            CHECK(target.is_empty_tile(tile<gate_layout>{0, 3}));
            CHECK(target.is_empty_tile(tile<gate_layout>{2, 3}));
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

            CHECK(target.num_pis() == 0);
            CHECK(target.num_gates() == 1);
            CHECK(target.num_pos() == 0);

            // Original PI/PO tiles are intentionally left open.
            CHECK(target.is_empty_tile(tile<gate_layout>{0, 2}));
            CHECK(target.is_empty_tile(tile<gate_layout>{2, 2}));
            CHECK(target.is_empty_tile(tile<gate_layout>{1, 4}));

            // Internal gate is copied and shifted.
            CHECK(target.is_and(target.get_node(tile<gate_layout>{1, 3})));
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

            CHECK(target.num_pis() == 0);
            CHECK(target.num_wires() == complex.num_wires() - complex.num_pis() - complex.num_pos());
            CHECK(target.num_gates() == complex.num_gates());
            CHECK(target.num_pos() == 0);

            // Check all coordinates shifted by 4
            CHECK(target.is_empty_tile(tile<gate_layout>{0, 4}));
            CHECK(target.is_empty_tile(tile<gate_layout>{2, 4}));
            CHECK(target.is_empty_tile(tile<gate_layout>{1, 7}));
            CHECK(target.is_wire(target.get_node(tile<gate_layout>{0, 5})));
            CHECK(target.is_wire(target.get_node(tile<gate_layout>{2, 5})));
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

            CHECK(target.num_pis() == 0);
            CHECK(target.num_gates() == 3);
            CHECK(target.num_pos() == 0);

            // Verify former PI/PO tiles are free.
            CHECK(target.is_empty_tile(tile<gate_layout>{0, 5}));
            CHECK(target.is_empty_tile(tile<gate_layout>{2, 5}));
            CHECK(target.is_empty_tile(tile<gate_layout>{4, 5}));
            CHECK(target.is_empty_tile(tile<gate_layout>{1, 8}));
        }

        SECTION("Layout with inverted signals")
        {
            gate_layout inv_layout{{4, 4}, row_clocking<gate_layout>()};
            const auto  pi1  = inv_layout.create_pi("x", {0, 0});
            const auto  inv1 = inv_layout.create_not(pi1, {0, 1});
            inv_layout.create_po(inv1, "not_x", {0, 2});

            auto target = detail::create_extended_layout(inv_layout, 3, 0);
            detail::copy_layout_with_offset(inv_layout, target, 3);

            CHECK(target.num_pis() == 0);
            CHECK(target.num_gates() == 1);
            CHECK(target.num_pos() == 0);

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

            CHECK(target.num_pis() == 0);
            CHECK(target.num_wires() == zero_offset.num_wires() - zero_offset.num_pis() - zero_offset.num_pos());
            CHECK(target.num_pos() == 0);

            // Former PI/PO tiles are free while internal wire keeps its position.
            CHECK(target.is_empty_tile(tile<gate_layout>{0, 0}));
            CHECK(target.is_empty_tile(tile<gate_layout>{0, 2}));
            CHECK(target.is_wire(target.get_node(tile<gate_layout>{0, 1})));
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
                std::abs(static_cast<int32_t>(obj.objective.source.x) - static_cast<int32_t>(obj.objective.target.x)));
        };

        CHECK(distance(objectives[0]) >= distance(objectives[1]));
        CHECK(distance(objectives[1]) >= distance(objectives[2]));

        SECTION("Targets shifted by pi_rows")
        {
            for (const auto& item : objectives)
            {
                CHECK(item.objective.target.y >= 4);
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
        CHECK(objectives[0].objective.source.y == 0);
        CHECK(objectives[0].objective.target.y >= 3);
    }

    SECTION("create_po_routing_objectives")
    {
        const std::vector desired{layout.get_node(po2), layout.get_node(po1), layout.get_node(po3)};
        auto              new_layout = detail::create_extended_layout(layout, 2, 4);

        detail::copy_layout_with_offset(layout, new_layout, 2);

        const auto objectives = detail::create_po_routing_objectives(layout, new_layout, current_pos, desired, 2, 4);

        REQUIRE(objectives.size() == 3);
        CHECK(new_layout.num_pos() == 0);

        const auto distance = [](const auto& obj)
        {
            return static_cast<uint32_t>(
                std::abs(static_cast<int32_t>(obj.objective.source.x) - static_cast<int32_t>(obj.objective.target.x)));
        };

        CHECK(distance(objectives[0]) >= distance(objectives[1]));
        CHECK(distance(objectives[1]) >= distance(objectives[2]));

        for (const auto& item : objectives)
        {
            CHECK(item.objective.source.y == 12);
            CHECK(item.objective.target.y == 16);
            CHECK_FALSE(new_layout.is_empty_tile(item.objective.source));
        }
    }

    SECTION("route_pi_objectives_with_a_star")
    {
        const std::vector desired{layout.get_node(x2), layout.get_node(x1), layout.get_node(x3)};
        auto              new_layout = detail::create_extended_layout(layout, 4, 0);

        detail::copy_layout_with_offset(layout, new_layout, 4);

        const auto objectives = detail::create_pi_routing_objectives(layout, new_layout, current_pis, desired, 4);
        REQUIRE(objectives.size() == 3);

        const std::filesystem::path dot_dir{"build/test_dot_layouts"};
        std::filesystem::create_directories(dot_dir);
        debug::write_dot_layout<gate_layout, gate_layout_hexagonal_drawer<gate_layout, false, false>>(
            new_layout, "route_pi_objectives_with_a_star_before_routing", dot_dir);

        detail::route_pi_objectives_with_a_star(new_layout, objectives);
        CHECK(new_layout.num_pis() == 3);
    }

    SECTION("route_po_objectives_with_a_star_and_create_pos")
    {
        const std::vector desired{layout.get_node(po2), layout.get_node(po1), layout.get_node(po3)};
        auto              new_layout = detail::create_extended_layout(layout, 2, 4);

        detail::copy_layout_with_offset(layout, new_layout, 2);

        const auto objectives = detail::create_po_routing_objectives(layout, new_layout, current_pos, desired, 2, 4);
        REQUIRE(objectives.size() == 3);

        const std::filesystem::path dot_dir{"build/test_dot_layouts"};
        std::filesystem::create_directories(dot_dir);
        debug::write_dot_layout<gate_layout, gate_layout_hexagonal_drawer<gate_layout, false, false>>(
            new_layout, "route_po_objectives_with_a_star_and_create_pos_before_routing", dot_dir);

        detail::route_po_objectives_with_a_star_and_create_pos(new_layout, objectives);

        CHECK(new_layout.num_pos() == 3);
        CHECK(new_layout.is_po_tile(objectives[0].objective.target));
        CHECK(new_layout.is_po_tile(objectives[1].objective.target));
        CHECK(new_layout.is_po_tile(objectives[2].objective.target));
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
                const auto unscrambled = unscramble_pins(layout, {}, target_pos);
                check_eq(layout, unscrambled);

                std::vector<std::string> expected_output_names{};
                expected_output_names.reserve(target_pos.size());

                for (const auto& target_po : target_pos)
                {
                    const auto current_it = std::find(pos.cbegin(), pos.cend(), target_po);
                    REQUIRE(current_it != pos.cend());
                    expected_output_names.push_back(
                        layout.get_output_name(static_cast<uint32_t>(std::distance(pos.cbegin(), current_it))));
                }

                std::vector<std::string> actual_output_names{};
                actual_output_names.reserve(unscrambled.num_pos());

                uint32_t po_index = 0u;
                unscrambled.foreach_po([&unscrambled, &actual_output_names, &po_index](const auto&)
                                       { actual_output_names.push_back(unscrambled.get_output_name(po_index++)); });

                CHECK(actual_output_names == expected_output_names);
            } while (std::next_permutation(target_pos.begin(), target_pos.end()));
        }
    }

    SECTION("Larger layout sampled permutations")
    {
        using hex_layout = hex_even_row_gate_clk_lyt;

        const auto ntk    = blueprints::clpl<technology_network>();
        const auto layout = generate_extended_hex_layout_from_network(ntk);

        std::vector<mockturtle::node<hex_layout>> pis{};
        pis.reserve(layout.num_pis());
        layout.foreach_pi([&pis](const auto& pi) { pis.push_back(pi); });

        std::vector<mockturtle::node<hex_layout>> pos{};
        pos.reserve(layout.num_pos());
        layout.foreach_po([&layout, &pos](const auto& po) { pos.push_back(layout.get_node(po)); });

        REQUIRE(pis.size() == 5);
        REQUIRE(pos.size() == 2);

        const auto sampled_permutations = [](const auto& ordering)
        {
            using node_t = typename std::decay_t<decltype(ordering)>::value_type;

            std::vector<std::vector<node_t>> samples{};

            const auto add_unique = [&samples](const std::vector<node_t>& candidate)
            {
                if (std::find(samples.cbegin(), samples.cend(), candidate) == samples.cend())
                {
                    samples.push_back(candidate);
                }
            };

            add_unique(ordering);

            auto reversed = ordering;
            std::reverse(reversed.begin(), reversed.end());
            add_unique(reversed);

            if (ordering.size() >= 2)
            {
                auto endpoints_swapped = ordering;
                std::swap(endpoints_swapped.front(), endpoints_swapped.back());
                add_unique(endpoints_swapped);
            }

            if (ordering.size() >= 3)
            {
                auto rotated_left = ordering;
                std::rotate(rotated_left.begin(), std::next(rotated_left.begin()), rotated_left.end());
                add_unique(rotated_left);
            }

            return samples;
        };

        const auto pi_samples = sampled_permutations(pis);
        const auto po_samples = sampled_permutations(pos);

        SECTION("Sampled PI permutations")
        {
            for (const auto& target_pis : pi_samples)
            {
                check_eq(layout, unscramble_pins(layout, target_pis, {}));
            }
        }

        SECTION("Sampled PO permutations")
        {
            for (const auto& target_pos : po_samples)
            {
                check_eq(layout, unscramble_pins(layout, {}, target_pos));
            }
        }

        SECTION("Sampled combined PI/PO permutations")
        {
            const auto num_samples = std::min(pi_samples.size(), po_samples.size());

            for (size_t i = 0; i < num_samples; ++i)
            {
                check_eq(layout, unscramble_pins(layout, pi_samples[i], po_samples[i]));
            }
        }
    }
}
