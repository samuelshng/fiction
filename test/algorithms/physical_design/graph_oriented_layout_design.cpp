//
// Created by simon on 12.06.24.
//

#include <catch2/catch_test_macros.hpp>

#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"

#include <fiction/algorithms/network_transformation/technology_mapping.hpp>
#include <fiction/algorithms/physical_design/apply_gate_library.hpp>
#include <fiction/algorithms/physical_design/graph_oriented_layout_design.hpp>
#include <fiction/layouts/cartesian_layout.hpp>
#include <fiction/layouts/cell_level_layout.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/netlist.hpp>
#include <fiction/networks/technology_network.hpp>
#include <fiction/technology/qca_one_library.hpp>
#include <fiction/utils/network_utils.hpp>
#include <fiction/utils/truth_table_utils.hpp>

#include <mockturtle/networks/aig.hpp>
#include <mockturtle/networks/mig.hpp>
#include <mockturtle/views/names_view.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fiction;

template <typename Lyt, typename Ntk>
void check_graph_oriented_layout_design_equiv(const Ntk& ntk)
{
    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                                = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
    params.cost                                = graph_oriented_layout_design_params::cost_objective::WIRES;
    params.enable_multithreading               = true;
    params.seed                                = 0u;
    params.timeout                             = 10000u;
    params.tiles_to_skip_between_pis           = 0u;
    params.randomize_tiles_to_skip_between_pis = true;
    params.return_first                        = false;

    const auto layout = graph_oriented_layout_design<Lyt>(ntk, params, &stats);
    REQUIRE(layout.has_value());

    check_eq(ntk, *layout);
}

template <typename Lyt>
void check_graph_oriented_layout_design_equiv_all()
{
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::maj1_network<mockturtle::aig_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::maj4_network<mockturtle::aig_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::unbalanced_and_inv_network<mockturtle::aig_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::and_or_network<technology_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::half_adder_network<mockturtle::mig_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::full_adder_network<technology_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::mux21_network<mockturtle::xag_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::se_coloring_corner_case_network<technology_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(
        blueprints::fanout_substitution_corner_case_network<technology_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::inverter_network<technology_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::clpl<technology_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(
        blueprints::one_to_five_path_difference_network<technology_network>());
    check_graph_oriented_layout_design_equiv<Lyt>(blueprints::nand_xnor_network<technology_network>());
}

mockturtle::names_view<fiction::netlist> multioutput_half_adder_fanout_network()
{
    mockturtle::names_view<fiction::netlist> ntk{};

    const auto a = ntk.create_pi("a");
    const auto b = ntk.create_pi("b");
    const auto c = ntk.create_pi("c");
    const auto d = ntk.create_pi("d");

    const auto ha = static_cast<mockturtle::block_network&>(ntk).create_node({a, b}, create_half_adder_tt());

    auto sum_output         = ha;
    sum_output.output       = 1u;
    const auto carry_output = ha;

    const auto sum_and_c = ntk.create_and(sum_output, c);
    const auto sum_or_d  = ntk.create_or(sum_output, d);
    const auto sum_xor_c = ntk.create_xor(sum_output, c);
    const auto sum_and_d = ntk.create_and(sum_output, d);

    ntk.create_po(sum_and_c, "sum_and_c");
    ntk.create_po(sum_or_d, "sum_or_d");
    ntk.create_po(sum_xor_c, "sum_xor_c");
    ntk.create_po(sum_and_d, "sum_and_d");
    ntk.create_po(carry_output, "carry");

    return ntk;
}

template <typename Lyt>
std::vector<std::string> collect_multioutput_launch_violations(const Lyt& lyt)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt must be a gate-level layout");
    static_assert(is_cartesian_layout_v<Lyt>, "Lyt must be a Cartesian layout");

    std::vector<std::string> violations{};

    lyt.foreach_gate(
        [&lyt, &violations](const auto& gate)
        {
            if (!lyt.is_multioutput(gate))
            {
                return;
            }

            const auto gate_tile = lyt.get_tile(gate);

            std::set<uint8_t>           used_output_pins{};
            std::array<std::string, 2u> pin_sides{};
            uint32_t                    east_fanouts  = 0u;
            uint32_t                    south_fanouts = 0u;
            uint32_t                    other_fanouts = 0u;
            bool                        inconsistent_pin_launch{false};

            lyt.foreach_fanout(gate,
                               [&lyt, &used_output_pins, &pin_sides, &east_fanouts, &south_fanouts, &other_fanouts,
                                &inconsistent_pin_launch, &gate_tile](const auto& fout)
                               {
                                   const auto fanout_tile = lyt.get_tile(fout);

                                   lyt.foreach_fanin(fout,
                                                     [&lyt, &used_output_pins, &pin_sides, &east_fanouts,
                                                      &south_fanouts, &other_fanouts, &inconsistent_pin_launch,
                                                      &gate_tile, &fanout_tile](const auto& fin)
                                                     {
                                                         if (static_cast<tile<Lyt>>(fin) != gate_tile)
                                                         {
                                                             return;
                                                         }

                                                         used_output_pins.insert(fin.output);

                                                         std::string side = "other";

                                                         if (fanout_tile == lyt.east(gate_tile))
                                                         {
                                                             ++east_fanouts;
                                                             side = "east";
                                                         }
                                                         else if (fanout_tile == lyt.south(gate_tile))
                                                         {
                                                             ++south_fanouts;
                                                             side = "south";
                                                         }
                                                         else
                                                         {
                                                             ++other_fanouts;
                                                         }

                                                         if (fin.output < pin_sides.size())
                                                         {
                                                             if (pin_sides[fin.output].empty())
                                                             {
                                                                 pin_sides[fin.output] = side;
                                                             }
                                                             else if (pin_sides[fin.output] != side)
                                                             {
                                                                 inconsistent_pin_launch = true;
                                                             }
                                                         }
                                                         else
                                                         {
                                                             inconsistent_pin_launch = true;
                                                         }
                                                     });
                               });

            if (used_output_pins.empty())
            {
                return;
            }

            const bool invalid_counts =
                (other_fanouts != 0u) || (east_fanouts > 1u) || (south_fanouts > 1u) ||
                ((used_output_pins.size() == 2u) && (east_fanouts != 1u || south_fanouts != 1u));

            if (inconsistent_pin_launch || invalid_counts)
            {
                std::ostringstream os{};
                os << "multi-output gate at (" << gate_tile.x << ", " << gate_tile.y << ", " << gate_tile.z
                   << ") uses invalid launch pattern: east=" << east_fanouts << ", south=" << south_fanouts
                   << ", other=" << other_fanouts << ", inconsistent_pin_launch=" << inconsistent_pin_launch;
                violations.push_back(os.str());
            }
        });

    return violations;
}

TEST_CASE("Layout equivalence after graph-oriented layout design", "[graph-oriented-layout-design]")
{
    SECTION("Cartesian layouts")
    {
        using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

        check_graph_oriented_layout_design_equiv_all<gate_layout>();
    }
}

TEST_CASE("Graph-oriented layout design preserves mapped half adder gate", "[graph-oriented-layout-design]")
{
    using gate_layout = cart_gate_clk_lyt;

    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    uint64_t mapped_num_ha_gates = 0;
    mapped_ha.foreach_gate(
        [&mapped_ha, &mapped_num_ha_gates](const auto& g)
        {
            if (mapped_ha.is_ha(g))
            {
                ++mapped_num_ha_gates;
            }
        });

    REQUIRE(mapped_num_ha_gates == 1);

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.timeout      = 100000;
    params.return_first = true;
    params.seed         = 0u;

    const auto layout = graph_oriented_layout_design<gate_layout>(mapped_ha, params, &stats);
    REQUIRE(layout.has_value());

    uint64_t layout_num_ha_gates = 0;
    layout->foreach_gate(
        [&layout_num_ha_gates, &layout](const auto& g)
        {
            if (layout->is_ha(g))
            {
                ++layout_num_ha_gates;
            }
        });

    CHECK(layout_num_ha_gates == 1);

    CHECK(layout->num_pos() == 2u);
    check_eq(mapped_ha, *layout);
    CHECK(collect_multioutput_launch_violations(*layout).empty());
}

TEST_CASE("Gate library application", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    using cell_layout = cell_level_layout<qca_technology, clocked_layout<cartesian_layout<offset::ucoord_t>>>;

    const auto check = [](const auto& ntk)
    {
        graph_oriented_layout_design_stats  stats{};
        graph_oriented_layout_design_params params{};
        params.timeout      = 100000;
        params.return_first = true;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());

        CHECK_NOTHROW(apply_gate_library<cell_layout, qca_one_library>(*layout));
    };

    check(blueprints::maj1_network<mockturtle::names_view<mockturtle::aig_network>>());
}

TEST_CASE("Graph-oriented layout design supports high-fanout multi-output gates", "[graph-oriented-layout-design]")
{
    using gate_layout = cart_gate_clk_lyt;
    const auto ntk    = multioutput_half_adder_fanout_network();

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
    params.timeout      = 100000u;
    params.seed         = 0u;
    params.return_first = true;

    const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
    REQUIRE(layout.has_value());

    CHECK(layout->num_pis() == ntk.num_pis());
    CHECK(layout->num_pos() == ntk.num_pos());
    CHECK(layout->num_gates() >= ntk.num_gates());
    check_eq(ntk, *layout);
    CHECK(collect_multioutput_launch_violations(*layout).empty());
}

TEST_CASE("Different parameters", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    const auto ntk    = blueprints::mux21_network<technology_network>();

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.return_first = true;

    SECTION("High-efficiency mode, return first found layout")
    {
        params.mode = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Verbose mode with timeout")
    {
        params.mode    = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
        params.timeout = 100000;
        params.verbose = true;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("High-effort mode")
    {
        params.mode    = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
        params.verbose = false;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Highest-effort mode")
    {
        params.mode = graph_oriented_layout_design_params::effort_mode::HIGHEST_EFFORT;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Maximum-effort mode")
    {
        params.mode = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Maximum-effort mode with random seed")
    {
        params.mode = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
        params.seed = 12345;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("More vertex expansions (num_vertex_expansions = 8)")
    {
        params.mode                  = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
        params.num_vertex_expansions = 8;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Straight inverters")
    {
        params.mode                  = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
        params.enable_multithreading = true;
        params.straight_inverters    = true;

        for (const auto& network :
             {blueprints::mux21_network<technology_network>(), blueprints::inverter_network<technology_network>(),
              blueprints::parity_network<technology_network>()})
        {
            const auto layout = graph_oriented_layout_design<gate_layout>(network, params, &stats);
            REQUIRE(layout.has_value());
            check_eq(network, *layout);

            layout->foreach_gate(
                [&layout](const auto& gate)
                {
                    if (layout->is_inv(gate))
                    {
                        const auto layout_tile = layout->get_tile(gate);
                        const auto fanin       = layout->incoming_data_flow(layout_tile).front();
                        const auto fanout      = layout->outgoing_data_flow(layout_tile).front();

                        const bool vertical_straight_inverter = (fanin.x == layout_tile.x && layout_tile.x == fanout.x);
                        const bool horizontal_straight_inverter =
                            (fanin.y == layout_tile.y && layout_tile.y == fanout.y);
                        const bool straight_inverter = vertical_straight_inverter || horizontal_straight_inverter;

                        CHECK(straight_inverter);
                    }
                });
        }
    }

    params.return_first = false;

    SECTION("Full search (return_first = false)")
    {
        params.mode = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Timeout limit reached")
    {
        params.mode    = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
        params.timeout = 0;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        CHECK(!layout.has_value());
    }

    SECTION("Planar layout (z = 0)")
    {
        params.mode    = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
        params.timeout = 100000;
        params.planar  = true;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
        CHECK(layout->z() == 0);
    }

    SECTION("Randomize skip tiles PI placement")
    {
        params.mode                                = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
        params.tiles_to_skip_between_pis           = 3;
        params.randomize_tiles_to_skip_between_pis = true;
        params.seed                                = 42;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Randomize skip tiles PI placement with zero value")
    {
        params.mode                                = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
        params.tiles_to_skip_between_pis           = 0;
        params.randomize_tiles_to_skip_between_pis = true;
        params.seed                                = 42;

        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }
}

TEST_CASE("Multithreading", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    const auto ntk    = blueprints::mux21_network<technology_network>();

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    // enable multithreading for all sections
    params.enable_multithreading = true;

    SECTION("Highest-effort mode with multithreading")
    {
        params.mode       = graph_oriented_layout_design_params::effort_mode::HIGHEST_EFFORT;
        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("High-efficiency mode, return first, multithreading")
    {
        params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
        params.return_first = true;
        const auto layout   = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }

    SECTION("Maximum-effort mode with seed and multithreading")
    {
        params.mode       = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
        params.seed       = 12345;
        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }
}

TEST_CASE("Different cost objectives", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    const auto ntk    = blueprints::mux21_network<technology_network>();

    graph_oriented_layout_design_stats stats{};

    graph_oriented_layout_design_params params{};

    params.mode = graph_oriented_layout_design_params::effort_mode::HIGHEST_EFFORT;

    // array of cost objectives to iterate over
    const std::array cost_objectives = {graph_oriented_layout_design_params::cost_objective::AREA,
                                        graph_oriented_layout_design_params::cost_objective::WIRES,
                                        graph_oriented_layout_design_params::cost_objective::CROSSINGS,
                                        graph_oriented_layout_design_params::cost_objective::ACP};

    // loop over each cost objective
    for (const auto& cost : cost_objectives)
    {
        params.cost       = cost;
        const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);

        REQUIRE(layout.has_value());
        check_eq(ntk, *layout);
    }
}

TEST_CASE("Skip tiles for PI placement", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

    const auto ntk = blueprints::clpl<technology_network>();

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};

    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
    params.return_first = true;

    for (uint64_t skip = 0; skip < 5; ++skip)
    {
        SECTION(fmt::format("tiles_to_skip_between_pis = {}", skip))
        {
            params.tiles_to_skip_between_pis = skip;

            const auto layout_opt = graph_oriented_layout_design<gate_layout>(ntk, params, &stats);
            REQUIRE(layout_opt.has_value());
            const auto& lyt = *layout_opt;
            check_eq(ntk, lyt);

            // collect PI coordinates along top (y=0) and left (x=0)
            std::vector<uint64_t> top_x, left_y;
            lyt.foreach_pi(
                [&](auto const& gate)
                {
                    const auto c = lyt.get_tile(gate);
                    if (c.y == 0)
                        top_x.push_back(c.x);
                    if (c.x == 0)
                        left_y.push_back(c.y);
                });

            std::sort(top_x.begin(), top_x.end());
            std::sort(left_y.begin(), left_y.end());

            // check gaps between consecutive PIs on each edge
            const auto min_gap = skip + 1;  // after placing a PI, leave `skip` empty tiles before next

            for (size_t i = 1; i < top_x.size(); ++i)
            {
                CAPTURE(skip, top_x);
                CHECK(top_x[i] >= top_x[i - 1] + min_gap);
            }
            for (size_t i = 1; i < left_y.size(); ++i)
            {
                CAPTURE(skip, left_y);
                CHECK(left_y[i] >= left_y[i - 1] + min_gap);
            }
        }
    }
}

TEST_CASE("Custom cost objective", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    const auto ntk    = blueprints::mux21_network<technology_network>();

    graph_oriented_layout_design_stats stats{};

    graph_oriented_layout_design_params params{};
    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGHEST_EFFORT;
    params.cost         = graph_oriented_layout_design_params::cost_objective::CUSTOM;
    params.return_first = true;

    // define a custom cost function
    const std::function<uint64_t(const gate_layout&)> custom_cost_objective = [](const gate_layout& layout) -> uint64_t
    {
        // Example custom logic for calculating cost
        return (layout.num_wires() * 2) + layout.num_crossings();
    };

    const auto layout = graph_oriented_layout_design<gate_layout>(ntk, params, &stats, custom_cost_objective);

    REQUIRE(layout.has_value());
    check_eq(ntk, *layout);

    // high-effort mode
    params.mode = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;

    const auto layout_high_effort =
        graph_oriented_layout_design<gate_layout>(ntk, params, &stats, custom_cost_objective);

    REQUIRE(layout_high_effort.has_value());
    check_eq(ntk, *layout_high_effort);

    // maximum-effort mode
    params.mode = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
    params.seed = 12345;

    const auto layout_maximum_effort =
        graph_oriented_layout_design<gate_layout>(ntk, params, &stats, custom_cost_objective);

    REQUIRE(layout_maximum_effort.has_value());
    check_eq(ntk, *layout_maximum_effort);
}

TEST_CASE("Name conservation after graph-oriented layout design", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

    auto maj = blueprints::maj1_network<mockturtle::aig_network>();
    maj.set_network_name("maj");

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.timeout      = 100000;
    params.return_first = true;
    params.seed         = 0u;

    const auto layout = graph_oriented_layout_design<gate_layout>(maj, params, &stats);

    REQUIRE(layout.has_value());

    // network name
    CHECK(layout->get_layout_name() == "maj");

    // PI names
    CHECK(layout->get_name(layout->pi_at(0)) == "a");  // first PI
    CHECK(layout->get_name(layout->pi_at(1)) == "b");  // second PI
    CHECK(layout->get_name(layout->pi_at(2)) == "c");  // third PI

    // PO names
    CHECK(layout->get_output_name(0) == "f");
}

TEST_CASE("High fanin exception", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    const auto ntk    = blueprints::maj1_network<technology_network>();

    graph_oriented_layout_design_stats stats{};

    graph_oriented_layout_design_params params{};

    CHECK_THROWS_AS(graph_oriented_layout_design<gate_layout>(ntk, params, &stats), high_degree_fanin_exception);
}

TEST_CASE("No custom cost objective provided exception", "[graph-oriented-layout-design]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    const auto ntk    = blueprints::mux21_network<technology_network>();

    graph_oriented_layout_design_stats stats{};

    graph_oriented_layout_design_params params{};
    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGHEST_EFFORT;
    params.cost         = graph_oriented_layout_design_params::cost_objective::CUSTOM;
    params.return_first = true;

    CHECK_THROWS_AS(graph_oriented_layout_design<gate_layout>(ntk, params, &stats), std::invalid_argument);
}
