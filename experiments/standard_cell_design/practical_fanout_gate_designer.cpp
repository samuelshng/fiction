//
// Design BitPlanarNet gate tiles.
//

#include "fiction_experiments.hpp"
#include "fanout_fom.hpp"

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

#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace fiction;

struct gate_design_request
{
    std::vector<tt> truth_tables;
    std::string name;
    std::size_t sidb_count;
};

int main(int argc, char* argv[])  // NOLINT
{
    using Lyt = sidb_100_cell_clk_lyt_siqad;

    // Configuration options
    bool find_all_solutions = false;  // Set to true to find all optimal gates
    bool save_to_file       = false;  // Set to true to save gate designs to files
    bool perform_fom        = false;  // Set to true to run FoM analysis on each solution
    std::filesystem::path output_directory = std::filesystem::current_path();  // Where to store gate designs if saved
    
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
        else if (arg == "--fom")
        {
            perform_fom = true;
        }
        else if (arg == "--output-dir" || arg == "-o")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: Missing path after " << arg << ".\n";
                return EXIT_FAILURE;
            }
            ++i;
            output_directory = argv[i];
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -a, --all     Find all optimal gate designs (default: first only)\n";
            std::cout << "  -s, --save    Save gate designs to .sqd files\n";
            std::cout << "  -o, --output-dir <path>\n"
                         "                 Directory to save designed gates (default: current working directory)\n";
            std::cout << "      --fom     Compute FoM metrics for each gate design\n";
            std::cout << "  -h, --help    Show this help message\n";
            return EXIT_SUCCESS;
        }
    }

    std::cout << "BitPlanarNet Gate Library Designer\n";
    std::cout << "Mode: " << (find_all_solutions ? "Find all optimal gates" : "Find first optimal gate") << "\n";
    std::cout << "Save to file: " << (save_to_file ? "Yes" : "No") << "\n";
    std::cout << "FoM analysis: " << (perform_fom ? "Enabled" : "Disabled") << "\n\n";
    if (save_to_file || perform_fom)
    {
        std::cout << "Output directory: " << output_directory.string() << "\n\n";
        if (!std::filesystem::exists(output_directory))
        {
            std::filesystem::create_directories(output_directory);
        }
    }

    // Define which gates to design (you can modify this list)
    // Each entry: {truth tables, gate name, number of SiDBs to place}
    const std::vector<gate_design_request> gates_to_design = {
        {create_false_fan_out_tt(), "false_fanout_2", 2},
        {create_false_fan_out_tt(), "false_fanout_3", 3},
        {create_nor_fan_out_tt(), "nor_fanout_3", 3},
        {create_nor_fan_out_tt(), "nor_fanout_4", 4},
        // {create_not_a_and_b_fan_out_tt(), "not_a_and_b_fanout", 3}, // MIRROR OF A AND NOT B
        {create_not_a_fan_out_tt(), "not_a_fanout_3", 3},
        {create_not_a_fan_out_tt(), "not_a_fanout_4", 4},
        {create_a_and_not_b_fan_out_tt(), "a_and_not_b_fanout_4", 4},
        // {create_not_b_fan_out_tt(), "not_b_fanout", 3}, // MIRROR OF NOT A
        {create_xor_fan_out_tt(), "xor_fanout_4", 4},
        {create_nand_fan_out_tt(), "nand_fanout_3", 3},
        {create_nand_fan_out_tt(), "nand_fanout_4", 4},
        {create_and_fan_out_tt(), "and_fanout_3", 3},
        {create_and_fan_out_tt(), "and_fanout_4", 4},
        {create_xnor_fan_out_tt(), "xnor_fanout_3", 3},
        {create_xnor_fan_out_tt(), "xnor_fanout_4", 4},
        // {create_b_fan_out_tt(), "b_fanout", 3}, // MIRROR OF A
        // {create_not_a_or_b_fan_out_tt(), "not_a_or_b_fanout", 3}, // MIRROR OF A OR NOT B
        {create_a_fan_out_tt(), "a_fanout_3", 3},
        {create_a_fan_out_tt(), "a_fanout_4", 4},
        {create_a_or_not_b_fan_out_tt(), "a_or_not_b_fanout_3", 3},
        {create_a_or_not_b_fan_out_tt(), "a_or_not_b_fanout_4", 4},
        {create_or_fan_out_tt(), "or_fanout_3", 3},
        {create_or_fan_out_tt(), "or_fanout_4", 4},
        {create_true_fan_out_tt(), "true_fanout_2", 2},
        {create_true_fan_out_tt(), "true_fanout_3", 3},
        // {create_half_adder_tt(), "half_adder_3", 3},
        // {create_half_adder_tt(), "half_adder_4", 4},
        {create_pass_left_xor_tt(), "pass_left_xor_4", 4},
        {create_pass_left_and_tt(), "pass_left_and_3", 3},
        {create_pass_left_and_tt(), "pass_left_and_4", 4},
        {create_pass_left_or_tt(), "pass_left_or", 3},
        {create_pass_left_or_tt(), "pass_left_or", 3},
        {create_and_or_tt(), "and_or_3", 3},
        {create_and_or_tt(), "and_or_4", 4},
        {create_demux_a_by_b_tt(), "demux_a_by_b_3", 3},
        {create_demux_a_by_b_tt(), "demux_a_by_b_4", 4},
        {create_gt_lt_tt(), "gt_lt_3", 3},
        {create_gt_lt_tt(), "gt_lt_4", 4},
        {create_a_not_a_tt(), "a_not_a_3", 3},
        {create_a_not_a_tt(), "a_not_a_4", 4},
        {create_crossing_wire_tt(), "crossing_wire_3", 3},
        {create_crossing_wire_tt(), "crossing_wire_4", 4},
        {create_double_wire_tt(), "double_wire_3", 3},
        {create_double_wire_tt(), "double_wire_4", 4},
    };

    static const std::string folder = fmt::format("{}/gate_skeletons/skeleton_bestagons_with_tags/", EXPERIMENTS_PATH);

    // Read the skeleton
    const auto skeleton = read_sqd_layout<Lyt>(
        fmt::format("{}/{}", folder, "skeleton_hex_inputsdbp_2i2o.sqd"));

    // Configure parameters for optimal gate finding
    const design_sidb_gates_params<fiction::cell<Lyt>> base_params{
        is_operational_params{sidb_simulation_parameters{2, -0.32}, sidb_simulation_engine::CLUSTERCOMPLETE,
                              bdl_input_iterator_params{detect_bdl_wires_params{3.0}},
                              is_operational_params::operational_condition::REJECT_KINKS},
        design_sidb_gates_params<fiction::cell<Lyt>>::design_sidb_gates_mode::QUICKCELL,
        {{14, 6, 0}, {24, 14, 0}},  // Canvas area
        3,  // Number of SiDBs to place
        find_all_solutions ? 
            design_sidb_gates_params<fiction::cell<Lyt>>::termination_condition::ALL_COMBINATIONS_ENUMERATED :
            design_sidb_gates_params<fiction::cell<Lyt>>::termination_condition::AFTER_FIRST_SOLUTION
    };

    // Design gates
    for (const auto& [truth_tables, gate_name, sidb_count] : gates_to_design)
    {
        std::cout << "Designing " << gate_name << " (" << sidb_count << " SiDB"
                  << (sidb_count == 1 ? "" : "s") << ")...\n";

        auto gate_params                    = base_params;
        gate_params.number_of_canvas_sidbs = sidb_count;
        
        design_sidb_gates_stats stats{};
        const auto gate_designs = design_sidb_gates(skeleton, truth_tables, gate_params, &stats);
        
        std::cout << "  Found " << gate_designs.size() << " optimal design(s)\n";
        std::cout << "  Runtime: " << mockturtle::to_seconds(stats.time_total) << " seconds\n";

        const auto gate_output_dir = output_directory / gate_name;
        if ((save_to_file || perform_fom) && !std::filesystem::exists(gate_output_dir))
        {
            std::filesystem::create_directories(gate_output_dir);
        }

        if (perform_fom)
        {
            const bool save_fom_report = save_to_file;
            static_cast<void>(fiction::experiments::compute_fom(gate_designs, truth_tables,
                                                                gate_params.operational_params, gate_name,
                                                                save_fom_report, gate_output_dir));
        }
        
        if (save_to_file && !gate_designs.empty())
        {
            if (find_all_solutions)
            {
                // Save all designs
                for (size_t i = 0; i < gate_designs.size(); ++i)
                {
                    const auto filename = fmt::format("{}_{}.sqd", gate_name, i);
                    const auto filepath = gate_output_dir / filename;
                    write_sqd_layout(gate_designs[i], filepath.string());
                    std::cout << "  Saved to " << filepath.string() << "\n";
                }
            }
            else
            {
                // Save only the first design
                const auto filename = fmt::format("{}.sqd", gate_name);
                const auto filepath = gate_output_dir / filename;
                write_sqd_layout(gate_designs.front(), filepath.string());
                std::cout << "  Saved to " << filepath.string() << "\n";
            }
        }
        
        std::cout << "\n";
    }

    std::cout << "Gate design complete!\n";
    
    return EXIT_SUCCESS;
} 
