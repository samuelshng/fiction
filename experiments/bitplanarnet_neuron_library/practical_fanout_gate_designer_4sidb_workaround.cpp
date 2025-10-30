//
// Workaround for 4-SiDB gate design without memory explosion
// Places one SiDB at each position, then runs QuickCell to find 3 more
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
#include <set>

using namespace fiction;

int main(int argc, char* argv[])  // NOLINT
{
    std::cout << "4-SiDB Gate Designer (Memory-Efficient Workaround)\n";
    std::cout << "Strategy: Place 1 SiDB, then use QuickCell to find 3 more\n\n";
    
    // Configuration
    bool save_to_file = false;
    bool find_all_per_position = false;  // If true, find all 3-SiDB solutions for each fixed position
    
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--save" || arg == "-s")
        {
            save_to_file = true;
        }
        else if (arg == "--all" || arg == "-a")
        {
            find_all_per_position = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -s, --save    Save gate designs to .sqd files\n";
            std::cout << "  -a, --all     Find all solutions per fixed position (default: first only)\n";
            std::cout << "  -h, --help    Show this help message\n";
            return EXIT_SUCCESS;
        }
    }

    const std::vector<std::pair<std::vector<tt>, std::string>> gates_to_design = {
        {create_a_and_not_b_fan_out_tt(), "a_and_not_b_fanout"},
        // {create_xor_fan_out_tt(), "xor_fanout"},
    };

    static const std::string folder = fmt::format("{}/gate_skeletons/skeleton_bestagons_with_tags/", EXPERIMENTS_PATH);

    // Read the skeleton
    const auto skeleton = read_sqd_layout<sidb_100_cell_clk_lyt_siqad>(
        fmt::format("{}/{}", folder, "skeleton_hex_inputsdbp_2i2o.sqd"));

    // Canvas parameters
    const fiction::coordinate<sidb_100_cell_clk_lyt_siqad> canvas_min{14, 6, 0};
    const fiction::coordinate<sidb_100_cell_clk_lyt_siqad> canvas_max{24, 14, 0};
    
    for (const auto& [truth_tables, gate_name] : gates_to_design)
    {
        std::cout << "Designing " << gate_name << " with 4 SiDBs...\n";
        
        std::vector<sidb_100_cell_clk_lyt_siqad> all_4sidb_designs;
        std::set<std::set<fiction::coordinate<sidb_100_cell_clk_lyt_siqad>>> unique_designs;
        
        // Iterate through all canvas positions for the first SiDB
        for (auto x = canvas_min.x; x <= canvas_max.x; ++x)
        {
            for (auto y = canvas_min.y; y <= canvas_max.y; ++y)
            {
                for (auto z = 0; z <= 1; ++z)  // Check both layers
                {
                    fiction::coordinate<sidb_100_cell_clk_lyt_siqad> fixed_pos{x, y, z};
                    
                    // Skip if position is occupied in skeleton
                    if (skeleton.get_cell_type(fixed_pos) != sidb_technology::cell_type::EMPTY)
                        continue;
                    
                    // Create skeleton with one fixed SiDB
                    auto skeleton_with_fixed = skeleton.clone();
                    skeleton_with_fixed.assign_cell_type(fixed_pos, sidb_technology::cell_type::LOGIC);
                    
                    std::cout << "  Fixed SiDB at (" << x << ", " << y << ", " << z << ")... ";
                    std::cout.flush();
                    
                    // Run QuickCell to find 3 more SiDBs
                    const design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>> params{
                        is_operational_params{sidb_simulation_parameters{2, -0.32}, sidb_simulation_engine::QUICKEXACT,
                                              bdl_input_iterator_params{detect_bdl_wires_params{3.0}},
                                              is_operational_params::operational_condition::REJECT_KINKS},
                        design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>>::design_sidb_gates_mode::QUICKCELL,
                        {canvas_min, canvas_max},
                        3,  // Find 3 more SiDBs (total of 4)
                        find_all_per_position ? 
                            design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>>::termination_condition::ALL_COMBINATIONS_ENUMERATED :
                            design_sidb_gates_params<fiction::cell<sidb_100_cell_clk_lyt_siqad>>::termination_condition::AFTER_FIRST_SOLUTION
                    };
                    
                    design_sidb_gates_stats stats{};
                    const auto gate_designs = design_sidb_gates(skeleton_with_fixed, truth_tables, params, &stats);
                    
                    if (!gate_designs.empty())
                    {
                        std::cout << "found " << gate_designs.size() << " design(s)\n";
                        
                        // Process each design
                        for (const auto& design : gate_designs)
                        {
                            // Extract all SiDB positions to check for duplicates
                            std::set<fiction::coordinate<sidb_100_cell_clk_lyt_siqad>> sidb_positions;
                            design.foreach_cell([&sidb_positions](const auto& c) {
                                sidb_positions.insert(c);
                            });
                            
                            // Only add if this is a unique design
                            if (unique_designs.find(sidb_positions) == unique_designs.end())
                            {
                                unique_designs.insert(sidb_positions);
                                all_4sidb_designs.push_back(design);
                                
                                if (save_to_file)
                                {
                                    const auto filename = fmt::format("{}_{}_4sidb.sqd", gate_name, all_4sidb_designs.size());
                                    write_sqd_layout(design, filename);
                                }
                            }
                        }
                    }
                    else
                    {
                        std::cout << "no designs found\n";
                    }
                    
                    // Early exit if we've found enough designs
                    if (!find_all_per_position && all_4sidb_designs.size() >= 10)
                    {
                        std::cout << "\n  Found sufficient designs, stopping early...\n";
                        goto done_searching;
                    }
                }
            }
        }
        
        done_searching:
        std::cout << "\nTotal unique 4-SiDB " << gate_name << " designs found: " << all_4sidb_designs.size() << "\n";
        if (save_to_file)
        {
            std::cout << "Saved to files: " << gate_name << "_1_4sidb.sqd through " 
                      << gate_name << "_" << all_4sidb_designs.size() << "_4sidb.sqd\n";
        }
    }
    
    std::cout << "\nWorkaround complete!\n";
    return EXIT_SUCCESS;
} 