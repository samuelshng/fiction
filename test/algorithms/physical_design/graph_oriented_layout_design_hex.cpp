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
#include <fiction/io/read_fgl_layout.hpp>
#include <fiction/io/network_reader.hpp>
#include <fiction/layouts/cartesian_layout.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>

#include <mockturtle/networks/aig.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fiction;

using cart_gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
using hex_gate_layout =
    gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

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

    INFO("projected pointy-top hex port violations:");
    for (const auto& violation : violations)
    {
        INFO(violation);
    }

    CHECK(violations.empty());
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
    check_projected_hex_port_legality(*layout);
    CHECK(layout->num_gates() >= mapped_ha.num_gates());
}

TEST_CASE("Mapped RCA2 with skipped PIs remains placeable on hex grid",
          "[graph-oriented-layout-design][graph-oriented-layout-design-hex]")
{
    const auto rca2_file_name = test::benchmark_path_utils::resolve("benchmarks/TOY/RCA2.v");

    std::ostringstream         os{};
    network_reader<aig_ptr>    reader{rca2_file_name, os};
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
    params.mode                       = graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT;
    params.seed                       = 0u;
    params.tiles_to_skip_between_pis  = 1u;
    params.timeout                    = 10000u;
    params.cost                       = graph_oriented_layout_design_params::cost_objective::AREA;

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
