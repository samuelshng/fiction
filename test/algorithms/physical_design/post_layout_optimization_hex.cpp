/**
 * @file post_layout_optimization_hex.cpp
 * @brief Tests for native post-layout optimization on row-clocked hexagonal layouts.
 */

#include <catch2/catch_test_macros.hpp>

#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"
#include "utils/hex_layout_port_legality.hpp"

#include <fiction/algorithms/network_transformation/technology_mapping.hpp>
#include <fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp>
#include <fiction/algorithms/physical_design/post_layout_optimization_hex.hpp>
#include <fiction/algorithms/verification/design_rule_violations.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/technology_network.hpp>

#include <mockturtle/networks/aig.hpp>

#include <cstdint>
#include <iostream>
#include <sstream>

using namespace fiction;

namespace
{

using hex_gate_layout =
    gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

template <typename Ntk>
std::optional<hex_gate_layout> run_gold_hex_native(const Ntk& ntk, graph_oriented_layout_design_params params,
                                                   graph_oriented_layout_design_stats* stats = nullptr)
{
    return graph_oriented_layout_design_hex<hex_gate_layout>(ntk, params, stats);
}

[[nodiscard]] hex_gate_layout make_mapped_half_adder_hex_gold_layout()
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

    auto layout = run_gold_hex_native(mapped_ha, params, &stats);
    REQUIRE(layout.has_value());
    check_eq(mapped_ha, *layout);

    return *layout;
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

TEST_CASE("Native hexagonal post-layout optimization preserves mapped hex GOLD half adder layouts",
          "[post_layout_optimization][post_layout_optimization_hex]")
{
    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    auto layout = make_mapped_half_adder_hex_gold_layout();
    layout.resize({layout.x() + 2u, layout.y() + 2u, layout.z()});

    const auto x_before    = layout.x() + 1u;
    const auto y_before    = layout.y() + 1u;
    const auto area_before = (layout.x() + 1u) * (layout.y() + 1u);

    post_layout_optimization_stats stats{};
    post_layout_optimization_hex(layout, {}, &stats);

    const auto area_after = (layout.x() + 1u) * (layout.y() + 1u);

    CHECK(layout.is_clocking_scheme(clock_name::ROW));
    CHECK(layout.x() + 1u <= x_before);
    CHECK(layout.y() + 1u <= y_before);
    CHECK(area_after <= area_before);
    CHECK(stats.x_size_before == x_before);
    CHECK(stats.y_size_before == y_before);
    CHECK(stats.num_wires_after <= stats.num_wires_before + 2u);
    CHECK(layout.num_pos() == 2u);
    check_projected_hex_port_legality(layout);
    check_eq(mapped_ha, layout);
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

TEST_CASE("Native hexagonal structural extraction preserves mapped half adder logic",
          "[post_layout_optimization][post_layout_optimization_hex][extraction]")
{
    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    const auto layout    = make_mapped_half_adder_hex_gold_layout();
    const auto extracted = detail::extract_structural_hex_network(layout);

    check_eq(mapped_ha, extracted);
    check_eq(aig_ha, extracted);
}

TEST_CASE("Native hexagonal GOLD fallback rebuild is safe for mapped half adder",
          "[post_layout_optimization][post_layout_optimization_hex][gold-fallback]")
{
    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    auto layout = make_mapped_half_adder_hex_gold_layout();

    if (layout.y() + 1u < 16u)
    {
        layout.resize({layout.x(), 15u, layout.z()});
    }

    const auto rebuilt = detail::try_hex_gold_rebuild(layout, {});

    REQUIRE(rebuilt.has_value());
    INFO("rebuilt size = " << rebuilt->x() + 1u << " x " << rebuilt->y() + 1u
                           << ", wires = " << rebuilt->num_wires() - rebuilt->num_pis() - rebuilt->num_pos());
    check_projected_hex_port_legality(*rebuilt);
    check_eq(mapped_ha, *rebuilt);
}
