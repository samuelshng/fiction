//
// Created by codex on 06.02.26.
//

#include <catch2/catch_test_macros.hpp>

#include "utils/benchmark_path_utils.hpp"
#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"
#include "utils/hex_layout_port_legality.hpp"

#include <fiction/algorithms/network_transformation/technology_mapping.hpp>
#include <fiction/algorithms/physical_design/graph_oriented_layout_design.hpp>
#include <fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp>
#include <fiction/io/network_reader.hpp>
#include <fiction/io/read_fgl_layout.hpp>
#include <fiction/layouts/cartesian_layout.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/netlist.hpp>
#include <fiction/utils/truth_table_utils.hpp>

#include <mockturtle/networks/aig.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fiction;

using cart_gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
using hex_gate_layout =
    gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

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

mockturtle::names_view<fiction::netlist> wide_shallow_pairwise_and_network(const uint64_t num_pairs)
{
    mockturtle::names_view<fiction::netlist> ntk{};

    std::vector<fiction::netlist::signal> pis{};
    pis.reserve(num_pairs * 2u);

    for (uint64_t i = 0u; i < num_pairs * 2u; ++i)
    {
        pis.push_back(ntk.create_pi(fmt::format("i{}", i)));
    }

    for (uint64_t pair = 0u; pair < num_pairs; ++pair)
    {
        const auto gate = ntk.create_and(pis[pair * 2u], pis[pair * 2u + 1u]);
        ntk.create_po(gate, fmt::format("o{}", pair));
    }

    return ntk;
}

void check_hex_io_placement(const hex_gate_layout& lyt)
{
    CHECK(lyt.is_clocking_scheme(clock_name::ROW));

    lyt.foreach_pi(
        [&lyt](const auto& pi)
        {
            const auto t = lyt.get_tile(pi);
            CHECK(t.y == 0);
        });

    lyt.foreach_po(
        [&lyt](const auto& po)
        {
            const auto t = lyt.get_tile(lyt.get_node(po));
            CHECK(t.y == lyt.y());
        });
}

/**
 * @brief Verifies that a native hex GOLD layout respects the strict one-connection-per-side projected port model.
 *
 * @param lyt Layout to inspect.
 */
void check_projected_hex_port_legality(const hex_gate_layout& lyt)
{
    const auto violations = test::hex_layout_port_legality::collect_port_violations(lyt);

    std::ostringstream os{};
    os << "projected pointy-top hex port violations:";

    for (const auto& violation : violations)
    {
        os << '\n' << violation;
    }

    INFO(os.str());
    CHECK(violations.empty());
}

std::vector<std::string> collect_multioutput_projected_launch_violations(const hex_gate_layout& lyt)
{
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
            uint32_t                    south_west_fanouts = 0u;
            uint32_t                    south_east_fanouts = 0u;
            uint32_t                    other_fanouts      = 0u;
            bool                        inconsistent_pin_launch{false};

            lyt.foreach_fanout(
                gate,
                [&lyt, &used_output_pins, &pin_sides, &south_west_fanouts, &south_east_fanouts, &other_fanouts,
                 &inconsistent_pin_launch, &gate_tile](const auto& fout)
                {
                    const auto fanout_tile = lyt.get_tile(fout);

                    lyt.foreach_fanin(
                        fout,
                        [&lyt, &used_output_pins, &pin_sides, &south_west_fanouts, &south_east_fanouts, &other_fanouts,
                         &inconsistent_pin_launch, &gate_tile, &fanout_tile](const auto& fin)
                        {
                            if (static_cast<tile<hex_gate_layout>>(fin) != gate_tile)
                            {
                                return;
                            }

                            used_output_pins.insert(fin.output);

                            std::string side = "other";

                            if (const auto projected_side =
                                    test::hex_layout_port_legality::outgoing_side(lyt, gate_tile, fanout_tile);
                                projected_side.has_value())
                            {
                                switch (*projected_side)
                                {
                                    case test::hex_layout_port_legality::projected_port_side::south_west:
                                        ++south_west_fanouts;
                                        side = "SW";
                                        break;
                                    case test::hex_layout_port_legality::projected_port_side::south_east:
                                        ++south_east_fanouts;
                                        side = "SE";
                                        break;
                                    default: ++other_fanouts; break;
                                }
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
                (other_fanouts != 0u) || (south_west_fanouts > 1u) || (south_east_fanouts > 1u) ||
                ((used_output_pins.size() == 2u) && (south_west_fanouts != 1u || south_east_fanouts != 1u));

            if (inconsistent_pin_launch || invalid_counts)
            {
                std::ostringstream os{};
                os << "multi-output gate at (" << gate_tile.x << ", " << gate_tile.y << ", " << gate_tile.z
                   << ") uses invalid projected launch pattern: SW=" << south_west_fanouts
                   << ", SE=" << south_east_fanouts << ", other=" << other_fanouts
                   << ", inconsistent_pin_launch=" << inconsistent_pin_launch;
                violations.push_back(os.str());
            }
        });

    return violations;
}

template <typename Ntk>
std::optional<hex_gate_layout>
run_gold_hex_native(const Ntk& ntk, graph_oriented_layout_design_params params,
                    graph_oriented_layout_design_stats*             stats       = nullptr,
                    std::function<uint64_t(const hex_gate_layout&)> custom_cost = nullptr)
{
    return graph_oriented_layout_design_hex<hex_gate_layout>(ntk, params, stats, custom_cost);
}

TEST_CASE("Mapped half adder flow on hexagonal grid",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
    params.return_first = true;
    params.seed         = 0u;
    params.timeout      = 100000u;

    const auto layout = run_gold_hex_native(mapped_ha, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);

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
    CHECK(collect_multioutput_projected_launch_violations(*layout).empty());
}

TEST_CASE("Mapped half adder flow on hexagonal grid with multithreading",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                  = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
    params.enable_multithreading = true;
    params.return_first          = false;
    params.seed                  = 0u;
    params.timeout               = 100000u;
    params.cost                  = graph_oriented_layout_design_params::cost_objective::WIRES;

    const auto layout = run_gold_hex_native(mapped_ha, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);
    check_eq(mapped_ha, *layout);
    CHECK(collect_multioutput_projected_launch_violations(*layout).empty());
}

TEST_CASE("Native hex GOLD supports high-fanout multi-output gates",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto ntk = multioutput_half_adder_fanout_network();

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
    params.return_first = true;
    params.seed         = 0u;
    params.timeout      = 100000u;
    params.cost         = graph_oriented_layout_design_params::cost_objective::WIRES;

    const auto layout = run_gold_hex_native(ntk, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);

    uint64_t layout_num_multioutput_gates = 0u;
    layout->foreach_gate(
        [&layout, &layout_num_multioutput_gates](const auto& g)
        {
            if (layout->is_multioutput(g))
            {
                ++layout_num_multioutput_gates;
            }
        });

    CHECK(layout->num_pis() == ntk.num_pis());
    CHECK(layout->num_pos() == ntk.num_pos());
    CHECK(layout_num_multioutput_gates == 1u);
    check_eq(ntk, *layout);
    CHECK(collect_multioutput_projected_launch_violations(*layout).empty());
}

TEST_CASE("Native hex GOLD handles wide shallow many-PI networks with PI gap",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto run_case =
        [](const uint64_t num_pairs, const uint64_t skip_tiles, const bool randomize_skip, const uint64_t timeout_ms)
    {
        const auto ntk = wide_shallow_pairwise_and_network(num_pairs);

        graph_oriented_layout_design_stats  cart_stats{};
        graph_oriented_layout_design_stats  hex_stats{};
        graph_oriented_layout_design_params params{};
        params.mode                                = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
        params.return_first                        = true;
        params.seed                                = 0u;
        params.timeout                             = timeout_ms;
        params.cost                                = graph_oriented_layout_design_params::cost_objective::AREA;
        params.num_vertex_expansions               = 1u;
        params.tiles_to_skip_between_pis           = skip_tiles;
        params.randomize_tiles_to_skip_between_pis = randomize_skip;
        params.prefer_input_pin_order              = true;
        params.prefer_output_pin_order             = true;

        const auto cart_layout = graph_oriented_layout_design<cart_gate_layout>(ntk, params, &cart_stats);
        REQUIRE(cart_layout.has_value());
        check_eq(ntk, *cart_layout);

        const auto hex_layout = run_gold_hex_native(ntk, params, &hex_stats);
        INFO("pairs=" << num_pairs << " skip=" << skip_tiles << " randomize=" << randomize_skip << " cart max_placed="
                      << cart_stats.max_placed_nodes << " cart_ssgs=" << cart_stats.num_search_space_graphs
                      << " hex max_placed=" << hex_stats.max_placed_nodes
                      << " hex_ssgs=" << hex_stats.num_search_space_graphs);
        REQUIRE(hex_layout.has_value());
        check_hex_io_placement(*hex_layout);
        check_projected_hex_port_legality(*hex_layout);
        check_eq(ntk, *hex_layout);
    };

    SECTION("12 pairs with gap 2")
    {
        run_case(12u, 2u, false, 15000u);
    }

    SECTION("16 pairs with randomized gap 1")
    {
        run_case(16u, 1u, true, 20000u);
    }
}

TEST_CASE("Projected port legality flags same-side stacked outputs on native hex GOLD layouts",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    static constexpr auto invalid_native_hex_fgl = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                                   "<fgl>\n"
                                                   "  <layout>\n"
                                                   "    <name>invalid_native_hex</name>\n"
                                                   "    <topology>even_row_hex</topology>\n"
                                                   "    <size>\n"
                                                   "      <x>1</x>\n"
                                                   "      <y>2</y>\n"
                                                   "      <z>1</z>\n"
                                                   "    </size>\n"
                                                   "    <clocking>\n"
                                                   "      <name>ROW</name>\n"
                                                   "    </clocking>\n"
                                                   "  </layout>\n"
                                                   "  <gates>\n"
                                                   "    <gate>\n"
                                                   "      <id>0</id>\n"
                                                   "      <type>PI</type>\n"
                                                   "      <name>a</name>\n"
                                                   "      <loc>\n"
                                                   "        <x>0</x>\n"
                                                   "        <y>0</y>\n"
                                                   "        <z>0</z>\n"
                                                   "      </loc>\n"
                                                   "    </gate>\n"
                                                   "    <gate>\n"
                                                   "      <id>1</id>\n"
                                                   "      <type>PI</type>\n"
                                                   "      <name>b</name>\n"
                                                   "      <loc>\n"
                                                   "        <x>1</x>\n"
                                                   "        <y>0</y>\n"
                                                   "        <z>0</z>\n"
                                                   "      </loc>\n"
                                                   "    </gate>\n"
                                                   "    <gate>\n"
                                                   "      <id>2</id>\n"
                                                   "      <type>HA</type>\n"
                                                   "      <loc>\n"
                                                   "        <x>1</x>\n"
                                                   "        <y>1</y>\n"
                                                   "        <z>0</z>\n"
                                                   "      </loc>\n"
                                                   "      <incoming>\n"
                                                   "        <signal>\n"
                                                   "          <x>0</x>\n"
                                                   "          <y>0</y>\n"
                                                   "          <z>0</z>\n"
                                                   "          <p>0</p>\n"
                                                   "        </signal>\n"
                                                   "        <signal>\n"
                                                   "          <x>1</x>\n"
                                                   "          <y>0</y>\n"
                                                   "          <z>0</z>\n"
                                                   "          <p>0</p>\n"
                                                   "        </signal>\n"
                                                   "      </incoming>\n"
                                                   "    </gate>\n"
                                                   "    <gate>\n"
                                                   "      <id>3</id>\n"
                                                   "      <type>BUF</type>\n"
                                                   "      <loc>\n"
                                                   "        <x>1</x>\n"
                                                   "        <y>2</y>\n"
                                                   "        <z>0</z>\n"
                                                   "      </loc>\n"
                                                   "      <incoming>\n"
                                                   "        <signal>\n"
                                                   "          <x>1</x>\n"
                                                   "          <y>1</y>\n"
                                                   "          <z>0</z>\n"
                                                   "          <p>0</p>\n"
                                                   "        </signal>\n"
                                                   "      </incoming>\n"
                                                   "    </gate>\n"
                                                   "    <gate>\n"
                                                   "      <id>4</id>\n"
                                                   "      <type>BUF</type>\n"
                                                   "      <loc>\n"
                                                   "        <x>1</x>\n"
                                                   "        <y>2</y>\n"
                                                   "        <z>1</z>\n"
                                                   "      </loc>\n"
                                                   "      <incoming>\n"
                                                   "        <signal>\n"
                                                   "          <x>1</x>\n"
                                                   "          <y>1</y>\n"
                                                   "          <z>0</z>\n"
                                                   "          <p>1</p>\n"
                                                   "        </signal>\n"
                                                   "      </incoming>\n"
                                                   "    </gate>\n"
                                                   "  </gates>\n"
                                                   "</fgl>\n";

    std::istringstream layout_stream{invalid_native_hex_fgl};
    const auto         layout = read_fgl_layout<hex_gate_layout>(layout_stream);

    const auto violations = test::hex_layout_port_legality::collect_port_violations(layout);

    CHECK_FALSE(violations.empty());
}

TEST_CASE("CLI-like gold hex options on mapped half adder",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                                = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
    params.enable_multithreading               = true;
    params.seed                                = 0u;
    params.tiles_to_skip_between_pis           = 0u;
    params.randomize_tiles_to_skip_between_pis = true;
    params.timeout                             = 10000u;
    params.cost                                = graph_oriented_layout_design_params::cost_objective::WIRES;

    const auto layout = run_gold_hex_native(mapped_ha, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);
    CHECK(layout->num_pis() == mapped_ha.num_pis());
    CHECK(layout->num_pos() == mapped_ha.num_pos());
}

TEST_CASE("Custom cost objective wiring for hex flow",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
    params.cost         = graph_oriented_layout_design_params::cost_objective::CUSTOM;
    params.return_first = true;
    params.seed         = 0u;
    params.timeout      = 100000u;

    const std::function<uint64_t(const hex_gate_layout&)> custom_cost_objective = [](const hex_gate_layout& layout)
    { return (layout.num_wires() * 2) + layout.num_crossings(); };

    const auto layout = run_gold_hex_native(mapped_ha, params, &stats, custom_cost_objective);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);
    CHECK(layout->num_pos() == 2u);
    check_eq(mapped_ha, *layout);
    CHECK(collect_multioutput_projected_launch_violations(*layout).empty());
}

TEST_CASE("Mapped RCA2 with skipped PIs remains placeable on hex grid",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto rca2_file_name = test::benchmark_path_utils::resolve("benchmarks/TOY/RCA2.v");

    std::ostringstream      os{};
    network_reader<aig_ptr> reader{rca2_file_name, os};
    REQUIRE(os.str().empty());

    const auto networks = reader.get_networks();
    REQUIRE(networks.size() == 1u);

    technology_mapping_params map_params{};
    map_params.ha   = true;
    map_params.and2 = true;
    map_params.or2  = true;
    map_params.xor2 = true;
    map_params.inv  = true;

    auto mapped = technology_mapping(*networks.front(), map_params);

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                      = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
    params.seed                      = 0u;
    params.tiles_to_skip_between_pis = 1u;
    params.timeout                   = 10000u;
    params.cost                      = graph_oriented_layout_design_params::cost_objective::AREA;

    const auto layout = run_gold_hex_native(mapped, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);
    CHECK(layout->num_pis() == mapped.num_pis());
    CHECK(layout->num_pos() == mapped.num_pos());
}

TEST_CASE("Mapped RCA2 first native hex GOLD solution respects projected port legality",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto rca2_file_name = test::benchmark_path_utils::resolve("benchmarks/TOY/RCA2.v");

    std::ostringstream      os{};
    network_reader<aig_ptr> reader{rca2_file_name, os};
    REQUIRE(os.str().empty());

    const auto networks = reader.get_networks();
    REQUIRE(networks.size() == 1u);

    technology_mapping_params map_params{};
    map_params.ha   = true;
    map_params.and2 = true;
    map_params.or2  = true;
    map_params.xor2 = true;
    map_params.inv  = true;

    auto mapped = technology_mapping(*networks.front(), map_params);

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                      = graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT;
    params.return_first              = true;
    params.enable_multithreading     = true;
    params.seed                      = 0u;
    params.tiles_to_skip_between_pis = 1u;
    params.timeout                   = 10000u;
    params.cost                      = graph_oriented_layout_design_params::cost_objective::AREA;

    const auto layout = run_gold_hex_native(mapped, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);
}

TEST_CASE("Hex GOLD explicit input pin ordering can be preferred",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    mockturtle::names_view<technology_network> ntk{};

    const auto a = ntk.create_pi("a");
    const auto b = ntk.create_pi("b");
    const auto f = ntk.create_and(a, b);

    ntk.create_po(f, "f");

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                   = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
    params.return_first           = true;
    params.seed                   = 0u;
    params.timeout                = 100000u;
    params.prefer_input_pin_order = true;
    params.input_pin_order        = {"b", "a"};

    const auto layout = run_gold_hex_native(ntk, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);

    std::unordered_map<std::string, uint64_t> pi_x{};
    layout->foreach_pi(
        [&layout, &pi_x](const auto& pi)
        {
            REQUIRE(layout->has_name(pi));
            pi_x.emplace(layout->get_name(pi), layout->get_tile(pi).x);
        });

    REQUIRE(pi_x.count("a") == 1u);
    REQUIRE(pi_x.count("b") == 1u);
    CHECK(pi_x.at("b") < pi_x.at("a"));
}

TEST_CASE("Hex GOLD explicit PI order is validated", "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    mockturtle::names_view<technology_network> ntk{};

    const auto a = ntk.create_pi("a");
    const auto b = ntk.create_pi("b");
    const auto f = ntk.create_and(a, b);

    ntk.create_po(f, "f");

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                   = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
    params.return_first           = true;
    params.seed                   = 0u;
    params.timeout                = 100000u;
    params.prefer_input_pin_order = true;

    SECTION("Unknown PI name")
    {
        params.input_pin_order = {"a", "missing"};

        CHECK_THROWS_AS(run_gold_hex_native(ntk, params, &stats), std::invalid_argument);
    }

    SECTION("Duplicate PI name")
    {
        params.input_pin_order = {"a", "a"};

        CHECK_THROWS_AS(run_gold_hex_native(ntk, params, &stats), std::invalid_argument);
    }
}

TEST_CASE("Hex GOLD explicit output pin ordering can be preferred",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    mockturtle::names_view<technology_network> ntk{};

    const auto a = ntk.create_pi("a");
    const auto b = ntk.create_pi("b");
    const auto f = ntk.create_and(a, b);

    ntk.create_po(f, "f");
    ntk.create_po(f, "g");

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                    = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
    params.return_first            = true;
    params.seed                    = 0u;
    params.timeout                 = 100000u;
    params.prefer_output_pin_order = true;
    params.output_pin_order        = {"g", "f"};

    const auto layout = run_gold_hex_native(ntk, params, &stats);
    REQUIRE(layout.has_value());
    check_hex_io_placement(*layout);
    check_projected_hex_port_legality(*layout);

    std::unordered_map<std::string, uint64_t> po_x{};
    layout->foreach_po(
        [&layout, &po_x](const auto& po, const auto index)
        {
            REQUIRE(layout->has_output_name(index));
            po_x.emplace(layout->get_output_name(index), layout->get_tile(layout->get_node(po)).x);
        });

    REQUIRE(po_x.count("f") == 1u);
    REQUIRE(po_x.count("g") == 1u);
    CHECK(po_x.at("g") < po_x.at("f"));
}

TEST_CASE("Hex GOLD explicit PO order is validated", "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    mockturtle::names_view<technology_network> ntk{};

    const auto a = ntk.create_pi("a");
    const auto b = ntk.create_pi("b");
    const auto f = ntk.create_and(a, b);

    ntk.create_po(f, "f");
    ntk.create_po(f, "g");

    graph_oriented_layout_design_stats  stats{};
    graph_oriented_layout_design_params params{};
    params.mode                    = graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY;
    params.return_first            = true;
    params.seed                    = 0u;
    params.timeout                 = 100000u;
    params.prefer_output_pin_order = true;

    SECTION("Unknown PO name")
    {
        params.output_pin_order = {"f", "missing"};

        CHECK_THROWS_AS(run_gold_hex_native(ntk, params, &stats), std::invalid_argument);
    }

    SECTION("Duplicate PO name")
    {
        params.output_pin_order = {"f", "f"};

        CHECK_THROWS_AS(run_gold_hex_native(ntk, params, &stats), std::invalid_argument);
    }
}

TEST_CASE("Exceptions in hex flow wrapper", "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    SECTION("No custom cost objective provided exception")
    {
        const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

        technology_mapping_stats mapping_stats{};
        const auto mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
        REQUIRE(!mapping_stats.mapper_stats.mapping_error);

        graph_oriented_layout_design_stats  stats{};
        graph_oriented_layout_design_params params{};
        params.mode         = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
        params.cost         = graph_oriented_layout_design_params::cost_objective::CUSTOM;
        params.return_first = true;

        CHECK_THROWS_AS(run_gold_hex_native(mapped_ha, params, &stats), std::invalid_argument);
    }
}
