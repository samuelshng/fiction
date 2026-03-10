/**
 * @file orthogonal_hex.cpp
 * @brief Tests for the native hexagonal orthogonal physical design flow.
 */

#include <catch2/catch_test_macros.hpp>

#include "utils/benchmark_path_utils.hpp"
#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"
#include "utils/hex_layout_port_legality.hpp"

#include <fiction/algorithms/network_transformation/technology_mapping.hpp>
#include <fiction/algorithms/physical_design/orthogonal_hex.hpp>
#include <fiction/io/network_reader.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/technology_network.hpp>

#include <mockturtle/networks/aig.hpp>
#include <mockturtle/networks/mig.hpp>
#include <mockturtle/views/names_view.hpp>

#include <sstream>

namespace
{

using namespace fiction;

template <typename Lyt>
void check_hex_border_io_placement(const Lyt& layout)
{
    layout.foreach_pi([&layout](const auto& pi) { CHECK(layout.get_tile(pi).y == 0u); });

    layout.foreach_po([&layout](const auto& po) { CHECK(layout.get_tile(layout.get_node(po)).y == layout.y()); });
}

template <typename Lyt>
void check_hex_downward_data_flow(const Lyt& layout)
{
    const auto is_upper_neighbor = [&layout](const auto& target, auto neighbor)
    {
        const auto base_target   = layout.below(target);
        const auto base_neighbor = layout.below(neighbor);

        return layout.north_east(base_target) == base_neighbor || layout.north_west(base_target) == base_neighbor;
    };

    const auto is_lower_neighbor = [&layout](const auto& source, auto neighbor)
    {
        const auto base_source   = layout.below(source);
        const auto base_neighbor = layout.below(neighbor);

        return layout.south_east(base_source) == base_neighbor || layout.south_west(base_source) == base_neighbor;
    };

    layout.foreach_node(
        [&layout, &is_upper_neighbor, &is_lower_neighbor](const auto& node)
        {
            if (layout.is_constant(node))
            {
                return;
            }

            const auto tile = layout.get_tile(node);

            for (const auto& incoming : layout.incoming_data_flow(tile))
            {
                CHECK(is_upper_neighbor(tile, static_cast<typename Lyt::tile>(incoming)));
            }

            for (const auto& outgoing : layout.outgoing_data_flow(tile))
            {
                CHECK(is_lower_neighbor(tile, static_cast<typename Lyt::tile>(outgoing)));
            }
        });
}

/**
 * @brief Verifies that a native hex layout does not reuse any projected top/bottom side more than once.
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

template <typename Lyt, typename Ntk>
void check_native_hex_ortho_equiv(const Ntk& ntk)
{
    orthogonal_physical_design_stats stats{};

    const auto layout = orthogonal_hex<Lyt>(ntk, {}, &stats);

    CHECK(stats.x_size > 0u);
    CHECK(stats.y_size > 0u);
    CHECK(stats.num_gates > 0u);
    CHECK(stats.num_wires > 0u);
    check_hex_border_io_placement(layout);
    check_hex_downward_data_flow(layout);
    check_eq(ntk, layout);
}

}  // namespace

TEST_CASE("Native hexagonal orthogonal layout equivalence", "[orthogonal][orthogonal-hex]")
{
    SECTION("odd row")
    {
        using gate_layout =
            gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

        SECTION("unbalanced_and_inv")
        {
            check_native_hex_ortho_equiv<gate_layout>(
                blueprints::unbalanced_and_inv_network<mockturtle::aig_network>());
        }
        SECTION("clpl")
        {
            check_native_hex_ortho_equiv<gate_layout>(blueprints::clpl<technology_network>());
        }
        SECTION("full_adder")
        {
            check_native_hex_ortho_equiv<gate_layout>(blueprints::full_adder_network<technology_network>());
        }
        SECTION("multi_output")
        {
            check_native_hex_ortho_equiv<gate_layout>(blueprints::multi_output_network<technology_network>());
        }
    }

    SECTION("even row")
    {
        using gate_layout =
            gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

        SECTION("unbalanced_and_inv")
        {
            check_native_hex_ortho_equiv<gate_layout>(
                blueprints::unbalanced_and_inv_network<mockturtle::aig_network>());
        }
        SECTION("clpl")
        {
            check_native_hex_ortho_equiv<gate_layout>(blueprints::clpl<technology_network>());
        }
        SECTION("full_adder")
        {
            check_native_hex_ortho_equiv<gate_layout>(blueprints::full_adder_network<technology_network>());
        }
        SECTION("multi_output")
        {
            check_native_hex_ortho_equiv<gate_layout>(blueprints::multi_output_network<technology_network>());
        }
    }
}

TEST_CASE("Name conservation after native hexagonal orthogonal physical design", "[orthogonal][orthogonal-hex]")
{
    using gate_layout =
        gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

    auto ntk = blueprints::unbalanced_and_inv_network<mockturtle::aig_network>();
    ntk.set_network_name("and_inv");

    const auto layout = orthogonal_hex<gate_layout>(ntk);

    CHECK(layout.get_layout_name() == "and_inv");
    CHECK(layout.get_name(layout.pi_at(0)) == "a");
    CHECK(layout.get_name(layout.pi_at(1)) == "b");
    CHECK(layout.get_output_name(0) == "f");
}

TEST_CASE("Native hexagonal orthogonal layout stays compact on RCA2", "[orthogonal][orthogonal-hex]")
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

    const auto mapped = technology_mapping(*networks.front(), map_params);

    orthogonal_physical_design_stats stats{};
    const auto                       layout = orthogonal_hex<gate_layout>(mapped, {}, &stats);

    CHECK(stats.x_size <= 16u);
    CHECK(stats.y_size <= 40u);
    CHECK(stats.num_wires <= 160u);
    CHECK(stats.num_crossings <= 32u);

    check_projected_hex_port_legality(layout);
    check_eq(mapped, layout);
    check_eq(*networks.front(), layout);
}
