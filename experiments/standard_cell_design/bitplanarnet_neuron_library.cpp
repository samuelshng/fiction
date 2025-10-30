//
// Created using quickcell_3_input.cpp as reference
//

#include "fiction_experiments.hpp"

#include <fiction/algorithms/iter/bdl_input_iterator.hpp>
#include <fiction/algorithms/physical_design/design_sidb_gates.hpp>
#include <fiction/algorithms/simulation/sidb/detect_bdl_wires.hpp>
#include <fiction/algorithms/simulation/sidb/is_operational.hpp>
#include <fiction/algorithms/simulation/sidb/sidb_simulation_engine.hpp>
#include <fiction/io/read_sqd_layout.hpp>
#include <fiction/traits.hpp>
#include <fiction/types.hpp>
#include <fiction/utils/truth_table_utils.hpp>

#include <fmt/format.h>
#include <mockturtle/utils/stopwatch.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace fiction;

// This script designs standard cells for 2-input Boolean functions with fanout using *QuickCell*. 
// The number of designed gate implementations and the required runtime are recorded.

int main()  // NOLINT
{
    experiments::experiment<std::string, uint64_t, uint64_t, double, uint64_t, double, uint64_t, double, uint64_t,
                            double, double>
        simulation_exp{
            "benchmark",
            "gate",                     // std::string
            "#Total Layouts",           // uint64_t
            "#Gates (QuickCell)",       // uint64_t
            "runtime (QuickCell) [s]",  // double
            "#Lp1",                     // uint64_t
            "#Lp1/N [%]",               // double
            "#Lp2",                     // uint64_t
            "#Lp2/N [%]",               // double
            "#Lp3",                     // uint64_t
            "#Lp3/N [%]",               // double
            "t_pruning [s]"             // double
        };

    // All 16 possible 2-input Boolean functions with fanout
    const auto truth_tables_and_names =
        std::array<std::pair<std::vector<tt>, std::string>, 16>{{
            {create_false_fan_out_tt(), "false_fanout"},
            {create_nor_fan_out_tt(), "nor_fanout"},
            {create_not_a_and_b_fan_out_tt(), "not_a_and_b_fanout"},
            {create_not_a_fan_out_tt(), "not_a_fanout"},
            {create_a_and_not_b_fan_out_tt(), "a_and_not_b_fanout"},
            {create_not_b_fan_out_tt(), "not_b_fanout"},
            {create_xor_fan_out_tt(), "xor_fanout"},
            {create_nand_fan_out_tt(), "nand_fanout"},
            {create_and_fan_out_tt(), "and_fanout"},
            {create_xnor_fan_out_tt(), "xnor_fanout"},
            {create_b_fan_out_tt(), "b_fanout"},
            {create_not_a_or_b_fan_out_tt(), "not_a_or_b_fanout"},
            {create_a_fan_out_tt(), "a_fanout"},
            {create_a_or_not_b_fan_out_tt(), "a_or_not_b_fanout"},
            {create_or_fan_out_tt(), "or_fanout"},
            {create_true_fan_out_tt(), "true_fanout"}
        }};

    static const std::string folder = fmt::format("{}/gate_skeletons/skeleton_bestagons_with_tags/", EXPERIMENTS_PATH);

    // Read the 2-input, 2-output skeleton
    const auto skeleton = read_sqd_layout<sidb_100_cell_clk_lyt_siqad>(
        fmt::format("{}/{}", folder, "skeleton_hex_inputsdbp_2i2o.sqd"));

    const design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>> params{
        is_operational_params{sidb_simulation_parameters{2, -0.32}, sidb_simulation_engine::QUICKEXACT,
                              bdl_input_iterator_params{detect_bdl_wires_params{3.0}},
                              is_operational_params::operational_condition::REJECT_KINKS},
        design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>>::design_sidb_gates_mode::QUICKCELL,
        {{14, 6, 0}, {24, 14, 0}},  // Adjusted canvas based on skeleton dimensions
        3};

    for (const auto& [truth_tables, gate_names] : truth_tables_and_names)
    {
        std::vector<sidb_100_cell_clk_lyt_siqad> quickcell_design{};
        design_sidb_gates_stats                  stats_quickcell{};

        quickcell_design = design_sidb_gates(skeleton, truth_tables, params, &stats_quickcell);

        const auto runtime_quickcell = mockturtle::to_seconds(stats_quickcell.time_total);

        const auto final_number_of_gates = quickcell_design.size();

        simulation_exp(gate_names, stats_quickcell.number_of_layouts, final_number_of_gates, runtime_quickcell,
                       stats_quickcell.number_of_layouts_after_first_pruning,
                       100.0 * static_cast<double>(stats_quickcell.number_of_layouts_after_first_pruning) /
                           static_cast<double>(stats_quickcell.number_of_layouts),
                       stats_quickcell.number_of_layouts_after_second_pruning,
                       100.0 * static_cast<double>(stats_quickcell.number_of_layouts_after_second_pruning) /
                           static_cast<double>(stats_quickcell.number_of_layouts),
                       stats_quickcell.number_of_layouts_after_third_pruning,
                       100.0 * static_cast<double>(stats_quickcell.number_of_layouts_after_third_pruning) /
                           static_cast<double>(stats_quickcell.number_of_layouts),
                       mockturtle::to_seconds(stats_quickcell.pruning_total));

        simulation_exp.save();
        simulation_exp.table();
    }

    simulation_exp.save();
    simulation_exp.table();

    return EXIT_SUCCESS;
}
