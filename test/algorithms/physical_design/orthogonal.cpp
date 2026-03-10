//
// Created by marcel on 02.06.21.
//

#include <catch2/catch_test_macros.hpp>

#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"

#include <fiction/algorithms/network_transformation/technology_mapping.hpp>
#include <fiction/algorithms/physical_design/apply_gate_library.hpp>
#include <fiction/algorithms/physical_design/orthogonal.hpp>
#include <fiction/layouts/cartesian_layout.hpp>
#include <fiction/layouts/cell_level_layout.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/layouts/gate_level_layout.hpp>
#include <fiction/layouts/tile_based_layout.hpp>
#include <fiction/networks/netlist.hpp>
#include <fiction/networks/technology_network.hpp>
#include <fiction/technology/qca_one_library.hpp>
#include <fiction/utils/truth_table_utils.hpp>

#include <mockturtle/networks/aig.hpp>
#include <mockturtle/networks/mig.hpp>
#include <mockturtle/views/fanout_view.hpp>
#include <mockturtle/views/names_view.hpp>

#include <array>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace fiction;

/**
 * @brief Creates a netlist with a multi-output half adder whose sum output fans out to several consumers.
 *
 * @return Named netlist containing one half adder and several dependent outputs.
 */
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

/**
 * @brief Checks whether multi-output gates launch their direct fanouts on valid Cartesian sides.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Cartesian gate-level layout.
 * @return Human-readable violations for invalid launch patterns.
 */
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

                                   lyt.foreach_fanin(
                                       fout,
                                       [&used_output_pins, &pin_sides, &east_fanouts, &south_fanouts, &other_fanouts,
                                        &inconsistent_pin_launch, &gate_tile, &fanout_tile](const auto& fin)
                                       {
                                           if (static_cast<tile<Lyt>>(fin) != gate_tile)
                                           {
                                               return;
                                           }

                                           used_output_pins.insert(fin.output);

                                           std::string side = "other";

                                           if (fanout_tile == tile<Lyt>{gate_tile.x + 1, gate_tile.y, gate_tile.z})
                                           {
                                               ++east_fanouts;
                                               side = "east";
                                           }
                                           else if (fanout_tile == tile<Lyt>{gate_tile.x, gate_tile.y + 1, gate_tile.z})
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

TEST_CASE("East-south coloring", "[orthogonal]")
{
    const auto check = [](const auto& ntk)
    {
        auto container = detail::east_south_edge_coloring(ntk);
        CHECK(detail::is_east_south_colored(container.color_ntk));
    };

    check(mockturtle::fanout_view{
        fanout_substitution<technology_network>(blueprints::unbalanced_and_inv_network<mockturtle::aig_network>())});
    check(mockturtle::fanout_view{
        fanout_substitution<technology_network>(blueprints::maj1_network<mockturtle::aig_network>())});
    check(mockturtle::fanout_view{
        fanout_substitution<technology_network>(blueprints::maj4_network<mockturtle::aig_network>())});
    check(mockturtle::fanout_view{
        fanout_substitution<technology_network>(blueprints::se_coloring_corner_case_network<technology_network>())});
    check(mockturtle::fanout_view{fanout_substitution<technology_network>(
        blueprints::fanout_substitution_corner_case_network<technology_network>())});
    check(mockturtle::fanout_view{
        fanout_substitution<technology_network>(blueprints::nary_operation_network<technology_network>())});
    check(mockturtle::fanout_view{fanout_substitution<technology_network>(blueprints::clpl<technology_network>())});
    check(mockturtle::fanout_view{
        fanout_substitution<technology_network>(blueprints::half_adder_network<mockturtle::mig_network>())});
    check(mockturtle::fanout_view{
        fanout_substitution<technology_network>(blueprints::full_adder_network<mockturtle::mig_network>())});
}

void check_stats(const orthogonal_physical_design_stats& st) noexcept
{
    CHECK(st.x_size > 0);
    CHECK(st.y_size > 0);
    CHECK(st.num_gates > 0);
    CHECK(st.num_wires > 0);
}

template <typename Lyt, typename Ntk>
void check_ortho_equiv(const Ntk& ntk)
{
    orthogonal_physical_design_stats stats{};

    auto layout = orthogonal<Lyt>(ntk, {}, &stats);

    check_stats(stats);
    check_eq(ntk, layout);
}

template <typename Lyt>
void check_ortho_equiv_all()
{
    check_ortho_equiv<Lyt>(blueprints::unbalanced_and_inv_network<mockturtle::aig_network>());
    check_ortho_equiv<Lyt>(blueprints::maj1_network<mockturtle::aig_network>());
    check_ortho_equiv<Lyt>(blueprints::maj4_network<mockturtle::aig_network>());
    check_ortho_equiv<Lyt>(blueprints::se_coloring_corner_case_network<technology_network>());
    check_ortho_equiv<Lyt>(blueprints::fanout_substitution_corner_case_network<technology_network>());
    check_ortho_equiv<Lyt>(blueprints::nary_operation_network<technology_network>());
    check_ortho_equiv<Lyt>(blueprints::clpl<technology_network>());

    // constant input network
    check_ortho_equiv<Lyt>(blueprints::unbalanced_and_inv_network<mockturtle::mig_network>());

    // multi-output network
    check_ortho_equiv<Lyt>(blueprints::multi_output_network<technology_network>());
}

TEST_CASE("Layout equivalence", "[algorithms]")
{
    SECTION("Cartesian layouts")
    {
        using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

        check_ortho_equiv_all<gate_layout>();
    }
    SECTION("Hexagonal layouts")
    {
        SECTION("odd row")
        {
            using gate_layout =
                gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_row_hex>>>>;

            check_ortho_equiv_all<gate_layout>();
        }
        SECTION("even row")
        {
            using gate_layout =
                gate_level_layout<clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_row_hex>>>>;

            check_ortho_equiv_all<gate_layout>();
        }
        SECTION("odd column")
        {
            using gate_layout = gate_level_layout<
                clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, odd_column_hex>>>>;

            check_ortho_equiv_all<gate_layout>();
        }
        SECTION("even column")
        {
            using gate_layout = gate_level_layout<
                clocked_layout<tile_based_layout<hexagonal_layout<offset::ucoord_t, even_column_hex>>>>;

            check_ortho_equiv_all<gate_layout>();
        }
    }
}

TEST_CASE("Gate library application", "[orthogonal]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;
    using cell_layout = cell_level_layout<qca_technology, clocked_layout<cartesian_layout<offset::ucoord_t>>>;

    const auto check = [](const auto& ntk)
    {
        orthogonal_physical_design_stats stats{};

        auto layout = orthogonal<gate_layout>(ntk, {}, &stats);

        CHECK_NOTHROW(apply_gate_library<cell_layout, qca_one_library>(layout));
    };

    check(blueprints::unbalanced_and_inv_network<mockturtle::aig_network>());
    check(blueprints::maj1_network<mockturtle::aig_network>());
    check(blueprints::maj4_network<mockturtle::aig_network>());
    check(blueprints::se_coloring_corner_case_network<technology_network>());
    check(blueprints::fanout_substitution_corner_case_network<technology_network>());
    check(blueprints::clpl<technology_network>());
    check(blueprints::half_adder_network<mockturtle::mig_network>());

    // constant input network
    check(blueprints::unbalanced_and_inv_network<mockturtle::mig_network>());
}

TEST_CASE("Name conservation after orthogonal physical design", "[orthogonal]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

    auto maj = blueprints::maj1_network<mockturtle::names_view<mockturtle::aig_network>>();
    maj.set_network_name("maj");

    const auto layout = orthogonal<gate_layout>(maj);

    // network name
    CHECK(layout.get_layout_name() == "maj");

    // PI names
    CHECK(layout.get_name(layout.pi_at(0)) == "a");  // first PI
    CHECK(layout.get_name(layout.pi_at(1)) == "b");  // second PI
    CHECK(layout.get_name(layout.pi_at(2)) == "c");  // third PI

    // PO names
    CHECK(layout.get_output_name(0) == "f");
}

TEST_CASE("Orthogonal preserves mapped half adder gate", "[orthogonal]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

    const auto aig_ha = blueprints::half_adder_network<mockturtle::aig_network>();

    technology_mapping_stats mapping_stats{};
    const auto               mapped_ha = technology_mapping(aig_ha, all_standard_2_input_functions(), &mapping_stats);
    REQUIRE(!mapping_stats.mapper_stats.mapping_error);

    const auto layout = orthogonal<gate_layout>(mapped_ha, {});

    uint64_t layout_num_ha_gates = 0u;
    layout.foreach_gate(
        [&layout, &layout_num_ha_gates](const auto& g)
        {
            if (layout.is_ha(g))
            {
                ++layout_num_ha_gates;
            }
        });

    CHECK(layout_num_ha_gates == 1u);
    CHECK(layout.num_pos() == 2u);
    check_eq(mapped_ha, layout);
    CHECK(collect_multioutput_launch_violations(layout).empty());
}

TEST_CASE("Orthogonal supports high-fanout multi-output gates", "[orthogonal]")
{
    using gate_layout = gate_level_layout<clocked_layout<tile_based_layout<cartesian_layout<offset::ucoord_t>>>>;

    const auto ntk    = multioutput_half_adder_fanout_network();
    const auto layout = orthogonal<gate_layout>(ntk, {});

    uint64_t layout_num_multioutput_gates = 0u;
    layout.foreach_gate(
        [&layout, &layout_num_multioutput_gates](const auto& g)
        {
            if (layout.is_multioutput(g))
            {
                ++layout_num_multioutput_gates;
            }
        });

    CHECK(layout.num_pis() == ntk.num_pis());
    CHECK(layout.num_pos() == ntk.num_pos());
    CHECK(layout_num_multioutput_gates == 1u);
    check_eq(ntk, layout);
    CHECK(collect_multioutput_launch_violations(layout).empty());
}
