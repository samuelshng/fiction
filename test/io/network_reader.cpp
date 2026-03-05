//
// Created by marcel on 27.09.21.
//

#include <catch2/catch_test_macros.hpp>

#include "utils/blueprints/network_blueprints.hpp"
#include "utils/equivalence_checking_utils.hpp"

#include <fiction/algorithms/physical_design/graph_oriented_layout_design.hpp>
#include <fiction/io/network_reader.hpp>
#include <fiction/types.hpp>

#include <cstdint>
#include <sstream>
#include <string>

using namespace fiction;

// adapted from https://stackoverflow.com/questions/44508228/c-how-to-check-if-ostringstream-is-empty
template <typename Stream>
bool is_stream_empty(Stream& stream)
{
    stream.flush();
    std::streampos pos = stream.tellp();    // store current location
    stream.seekp(0, std::ios_base::end);    // go to end
    bool is_empty = (stream.tellp() == 0);  // check size == 0 ?
    stream.seekp(pos);                      // restore location

    return is_empty;
}

uint64_t count_ha_gates(const tec_nt& ntk)
{
    uint64_t num_ha = 0u;

    ntk.foreach_gate(
        [&ntk, &num_ha](const auto& gate)
        {
            if (ntk.is_ha(gate))
            {
                ++num_ha;
            }
        });

    return num_ha;
}

TEST_CASE("Read Verilog", "[network-reader]")
{
    constexpr const char* mux21_file_name = "../../benchmarks/TOY/mux21.v";

    std::ostringstream os{};

    network_reader<aig_ptr> reader{mux21_file_name, os};

    // no error messages
    REQUIRE(is_stream_empty(os));

    const auto nets = reader.get_networks();

    // exactly one net should have been created
    REQUIRE(nets.size() == 1);

    const auto mux21 = *nets.front();

    SECTION("Equality")
    {
        check_eq(mux21, blueprints::mux21_network<aig_nt>());
    }
    SECTION("Name conservation")
    {
        // network name
        CHECK(mux21.get_network_name() == "mux21");

        // PI names
        CHECK(mux21.get_name(mux21.make_signal(1)) == "in0");  // first PI
        CHECK(mux21.get_name(mux21.make_signal(2)) == "in1");  // second PI
        CHECK(mux21.get_name(mux21.make_signal(3)) == "in2");  // third PI

        // PO names
        CHECK(mux21.get_output_name(0) == "out");
    }
}

TEST_CASE("Read mixed BLIF with explicit half adder gate", "[network-reader]")
{
    constexpr const char* ha_blif_file_name = "../../benchmarks/TOY/ha_gate_mixed.blif";

    std::ostringstream os{};

    network_reader<tec_ptr> reader{ha_blif_file_name, os};

    REQUIRE(is_stream_empty(os));

    const auto networks = reader.get_networks();
    REQUIRE(networks.size() == 1u);

    const auto& ntk = *networks.front();

    CHECK(ntk.num_pis() == 3u);
    CHECK(ntk.num_pos() == 3u);
    CHECK(ntk.num_gates() == 2u);
    CHECK(count_ha_gates(ntk) == 1u);

    graph_oriented_layout_design_params gold_params{};
    gold_params.timeout      = 100000u;
    gold_params.return_first = true;

    const auto layout = graph_oriented_layout_design<cart_gate_clk_lyt>(ntk, gold_params);
    REQUIRE(layout.has_value());
}

TEST_CASE("Read BLIF with .subckt half adder aliases", "[network-reader]")
{
    constexpr const char* ha_blif_file_name = "../../benchmarks/TOY/ha_subckt_alias.blif";

    std::ostringstream os{};

    network_reader<tec_ptr> reader{ha_blif_file_name, os};

    REQUIRE(is_stream_empty(os));

    const auto networks = reader.get_networks();
    REQUIRE(networks.size() == 1u);

    const auto& ntk = *networks.front();

    CHECK(ntk.num_pis() == 3u);
    CHECK(ntk.num_pos() == 3u);
    CHECK(ntk.num_gates() == 2u);
    CHECK(count_ha_gates(ntk) == 1u);
}
