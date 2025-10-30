//
// Practical SiDB gate designer for fanout gates
// This script is optimized for practical use, returning optimal gate designs efficiently
//

#include "fiction_experiments.hpp"

#include <fiction/algorithms/iter/bdl_input_iterator.hpp>
#include <fiction/algorithms/physical_design/design_sidb_gates.hpp>
#include <fiction/algorithms/simulation/sidb/detect_bdl_wires.hpp>
#include <fiction/algorithms/simulation/sidb/is_operational.hpp>
#include <fiction/algorithms/simulation/sidb/sidb_simulation_engine.hpp>
#include <fiction/io/read_sqd_layout.hpp>
#include <fiction/io/write_sqd_layout.hpp>
#include <fiction/traits.hpp>
#include <fiction/types.hpp>
#include <fiction/utils/truth_table_utils.hpp>

#include <fmt/format.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace fiction;

int main(int argc, char* argv[])  // NOLINT
{
    // Configuration options
    bool find_all_solutions = false;  // Set to true to find all optimal gates
    bool save_to_file = false;        // Set to true to save gate designs to files
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--all" || arg == "-a")
        {
            find_all_solutions = true;
        }
        else if (arg == "--save" || arg == "-s")
        {
            save_to_file = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -a, --all     Find all optimal gate designs (default: first only)\n";
            std::cout << "  -s, --save    Save gate designs to .sqd files\n";
            std::cout << "  -h, --help    Show this help message\n";
            return EXIT_SUCCESS;
        }
    }

    std::cout << "SiDB Fanout Gate Designer\n";
    std::cout << "Mode: " << (find_all_solutions ? "Find all optimal gates" : "Find first optimal gate") << "\n";
    std::cout << "Save to file: " << (save_to_file ? "Yes" : "No") << "\n\n";

    // Define which gates to design (you can modify this list)
    // const std::vector<std::pair<std::vector<tt>, std::string>> gates_to_design = {
    //     {create_false_fan_out_tt(), "false_fanout"},
    //     {create_nor_fan_out_tt(), "nor_fanout"},
    //     // {create_not_a_and_b_fan_out_tt(), "not_a_and_b_fanout"}, // MIRROR OF A AND NOT B
    //     {create_not_a_fan_out_tt(), "not_a_fanout"},
    //     {create_a_and_not_b_fan_out_tt(), "a_and_not_b_fanout"},
    //     // {create_not_b_fan_out_tt(), "not_b_fanout"}, // MIRROR OF NOT A
    //     {create_xor_fan_out_tt(), "xor_fanout"},
    //     {create_nand_fan_out_tt(), "nand_fanout"},
    //     {create_and_fan_out_tt(), "and_fanout"},
    //     {create_xnor_fan_out_tt(), "xnor_fanout"},
    //     // {create_b_fan_out_tt(), "b_fanout"}, // MIRROR OF A
    //     // {create_not_a_or_b_fan_out_tt(), "not_a_or_b_fanout"}, // MIRROR OF A OR NOT B
    //     {create_a_fan_out_tt(), "a_fanout"},
    //     {create_a_or_not_b_fan_out_tt(), "a_or_not_b_fanout"},
    //     {create_or_fan_out_tt(), "or_fanout"},
    //     {create_true_fan_out_tt(), "true_fanout"}
    // };
    const std::vector<std::pair<std::vector<tt>, std::string>> gates_to_design = {
        // {create_a_and_not_b_fan_out_tt(), "a_and_not_b_fanout"},
        {create_xor_fan_out_tt(), "xor_fanout"},
    };

    static const std::string folder = fmt::format("{}/gate_skeletons/skeleton_bestagons_with_tags/", EXPERIMENTS_PATH);

    // Read the skeleton
    const auto skeleton = read_sqd_layout<sidb_100_cell_clk_lyt_siqad>(
        fmt::format("{}/{}", folder, "skeleton_hex_inputsdbp_2i2o.sqd"));

    // Configure parameters for optimal gate finding
    // NOTE: Memory usage scales as C(canvas_size, num_sidbs)
    // - 3 SiDBs on 11x9 canvas: C(99,3) = 156,849 combinations (~16GB RAM)
    // - 4 SiDBs on 11x9 canvas: C(99,4) = 3,764,376 combinations (~80GB RAM)
    // - 4 SiDBs on 5x5 canvas: C(25,4) = 12,650 combinations (~1.3GB RAM)
    const design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>> params{
        is_operational_params{sidb_simulation_parameters{2, -0.32}, sidb_simulation_engine::QUICKEXACT,
                              bdl_input_iterator_params{detect_bdl_wires_params{3.0}},
                              is_operational_params::operational_condition::REJECT_KINKS},
        design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>>::design_sidb_gates_mode::QUICKCELL,
        {{14, 6, 0}, {24, 14, 0}},  // Original canvas area
        // Consider reducing canvas to {{16, 8, 0}, {22, 12, 0}} for 4 SiDBs to reduce memory usage
        3,  // Reverted to 3 SiDBs to avoid memory issues
        find_all_solutions ? 
            design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>>::termination_condition::ALL_COMBINATIONS_ENUMERATED :
            design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>>::termination_condition::AFTER_FIRST_SOLUTION
        // Using AFTER_FIRST_SOLUTION significantly reduces memory usage by stopping after finding the first valid gate
    };

    // Design gates
    for (const auto& [truth_tables, gate_name] : gates_to_design)
    {
        std::cout << "Designing " << gate_name << "...\n";
        
        design_sidb_gates_stats stats{};
        const auto gate_designs = design_sidb_gates(skeleton, truth_tables, params, &stats);
        
        std::cout << "  Found " << gate_designs.size() << " optimal design(s)\n";
        std::cout << "  Runtime: " << mockturtle::to_seconds(stats.time_total) << " seconds\n";
        
        if (save_to_file && !gate_designs.empty())
        {
            if (find_all_solutions)
            {
                // Save all designs
                for (size_t i = 0; i < gate_designs.size(); ++i)
                {
                    const auto filename = fmt::format("{}_{}.sqd", gate_name, i);
                    write_sqd_layout(gate_designs[i], filename);
                    std::cout << "  Saved to " << filename << "\n";
                }
            }
            else
            {
                // Save only the first design
                const auto filename = fmt::format("{}.sqd", gate_name);
                write_sqd_layout(gate_designs.front(), filename);
                std::cout << "  Saved to " << filename << "\n";
            }
        }
        
        std::cout << "\n";
    }

    std::cout << "Gate design complete!\n";
    
    return EXIT_SUCCESS;
} 