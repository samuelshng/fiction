/**
 * @file post_layout_optimization_hex.cpp
 * @brief Tests for native post-layout optimization on row-clocked hexagonal layouts.
 */

#include <catch2/catch_test_macros.hpp>

#include "utils/benchmark_path_utils.hpp"
#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"
#include "utils/hex_layout_port_legality.hpp"

#include <fiction/algorithms/network_transformation/technology_mapping.hpp>
#include <fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp>
#include <fiction/algorithms/physical_design/orthogonal_hex.hpp>
#include <fiction/algorithms/physical_design/post_layout_optimization_hex.hpp>
#include <fiction/algorithms/verification/design_rule_violations.hpp>
#include <fiction/io/network_reader.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/technology_network.hpp>

#include <cstdint>
#include <iostream>
#include <sstream>

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
    const auto area_before = (layout.x() + 1u) * (layout.y() + 1u);

    post_layout_optimization_stats stats{};
    post_layout_optimization_hex(layout, {}, &stats);

    const auto area_after = (layout.x() + 1u) * (layout.y() + 1u);

    CHECK(layout.is_clocking_scheme(clock_name::ROW));
    CHECK(area_after <= area_before);
    CHECK(stats.x_size_before == x_before);
    CHECK(stats.y_size_before == y_before);
    check_eq(ntk, layout);

    layout.foreach_pi([&layout](const auto& pi) { CHECK(layout.get_tile(pi).y == 0u); });

    layout.foreach_po([&layout](const auto& po) { CHECK(layout.get_tile(layout.get_node(po)).y == layout.y()); });
}

/**
 * @brief Checks that a layout does not violate projected pointy-top hex port constraints.
 *
 * @tparam Lyt Layout type.
 * @param layout Layout to inspect.
 */
template <typename Lyt>
void check_projected_hex_port_legality(const Lyt& layout)
{
    const auto violations = test::hex_layout_port_legality::collect_port_violations(layout);

    std::ostringstream os{};
    os << "projected pointy-top hex port violations:";

    for (const auto& violation : violations)
    {
        os << '\n' << violation;
    }

    INFO(os.str());
    CHECK(violations.empty());
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
        check_hex_optimization<odd_row_layout>(blueprints::full_adder_network<technology_network>(),
                                               [](const auto& ntk) { return orthogonal_hex<odd_row_layout>(ntk); });
    }

    SECTION("even row")
    {
        check_hex_optimization<even_row_layout>(blueprints::full_adder_network<technology_network>(),
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

TEST_CASE("Native hexagonal post-layout optimization keeps orthogonal RCA2 rewiring equivalent",
          "[post_layout_optimization][post_layout_optimization_hex][orthogonal-hex]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

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

    const auto mapped      = technology_mapping(*networks.front(), map_params);
    auto       layout      = orthogonal_hex<gate_layout>(mapped);
    const auto x_before    = layout.x() + 1u;
    const auto y_before    = layout.y() + 1u;
    const auto area_before = x_before * y_before;

    std::ostringstream             optimize_output{};
    const auto                     old_cout_buf = std::cout.rdbuf(optimize_output.rdbuf());
    post_layout_optimization_stats stats{};
    post_layout_optimization_hex(layout, {}, &stats);
    std::cout.rdbuf(old_cout_buf);

    gate_level_drv_params drv_params{};
    drv_params.out = &os;
    gate_level_drv_stats drv_stats{};
    gate_level_drvs(layout, drv_params, &drv_stats);

    const auto area_after = (layout.x() + 1u) * (layout.y() + 1u);

    CHECK(layout.is_clocking_scheme(clock_name::ROW));
    CHECK(layout.x() + 1u <= x_before);
    CHECK(layout.y() + 1u < y_before);
    CHECK(area_after < area_before);
    CHECK(stats.num_wires_after <= stats.num_wires_before + 2u);
    CHECK(optimize_output.str().find("discarded native hex rewiring candidate because it changed functionality") ==
          std::string::npos);
    CHECK(drv_stats.drvs == 0u);
    check_projected_hex_port_legality(layout);
    check_eq(mapped, layout);
    check_eq(*networks.front(), layout);
}

TEST_CASE("Native hexagonal structural extraction preserves orthogonal RCA2 logic",
          "[post_layout_optimization][post_layout_optimization_hex][orthogonal-hex][extraction]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

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

    const auto mapped            = technology_mapping(*networks.front(), map_params);
    const auto orthogonal_layout = orthogonal_hex<gate_layout>(mapped);
    const auto extracted         = detail::extract_structural_hex_network(orthogonal_layout);

    check_eq(mapped, extracted);
    check_eq(*networks.front(), extracted);
}

TEST_CASE("Native hexagonal GOLD fallback rebuild is safe for orthogonal RCA2",
          "[post_layout_optimization][post_layout_optimization_hex][orthogonal-hex][gold-fallback]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

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

    const auto mapped            = technology_mapping(*networks.front(), map_params);
    const auto orthogonal_layout = orthogonal_hex<gate_layout>(mapped);
    const auto rebuilt           = detail::try_hex_gold_rebuild(orthogonal_layout, {});

    REQUIRE(rebuilt.has_value());
    INFO("rebuilt size = " << rebuilt->x() + 1u << " x " << rebuilt->y() + 1u
                           << ", wires = " << rebuilt->num_wires() - rebuilt->num_pis() - rebuilt->num_pos());
    check_eq(mapped, *rebuilt);
}
