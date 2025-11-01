#include "fanout_fom.hpp"

#include <fiction/algorithms/iter/bdl_input_iterator.hpp>
#include <fiction/algorithms/simulation/sidb/detect_bdl_wires.hpp>
#include <fiction/algorithms/simulation/sidb/is_operational.hpp>
#include <fiction/algorithms/simulation/sidb/sidb_simulation_engine.hpp>
#include <fiction/algorithms/simulation/sidb/sidb_simulation_parameters.hpp>
#include <fiction/io/read_sqd_layout.hpp>
#include <fiction/types.hpp>
#include <fiction/utils/truth_table_utils.hpp>

#include <fmt/core.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace fiction;

namespace
{

using layout_t              = sidb_100_cell_clk_lyt_siqad;
using truth_table_generator = std::function<std::vector<tt>()>;

inline truth_table_generator make_tt_generator(std::vector<tt> (*fn)())
{
    return [fn]() { return fn(); };
}

inline truth_table_generator make_tt_generator(tt (*fn)())
{
    return [fn]() { return std::vector<tt>{fn()}; };
}

bool is_numeric_string(const std::string& text)
{
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](const unsigned char c) { return std::isdigit(c) != 0; });
}

std::vector<std::string> candidate_gate_names(std::string base)
{
    std::vector<std::string> candidates{};
    candidates.push_back(base);

    while (true)
    {
        const auto pos = base.find_last_of('_');
        if (pos == std::string::npos)
        {
            break;
        }
        const std::string suffix = base.substr(pos + 1);
        if (!is_numeric_string(suffix))
        {
            break;
        }
        base = base.substr(0, pos);
        candidates.push_back(base);
    }

    return candidates;
}

const std::unordered_map<std::string, truth_table_generator>& truth_table_lookup()
{
    static const std::unordered_map<std::string, truth_table_generator> lookup = {
        {"a_and_not_b_fanout", make_tt_generator(&create_a_and_not_b_fan_out_tt)},
        {"a_fanout", make_tt_generator(&create_a_fan_out_tt)},
        {"a_not_a", make_tt_generator(&create_a_not_a_tt)},
        {"a_or_not_b_fanout", make_tt_generator(&create_a_or_not_b_fan_out_tt)},
        {"and", make_tt_generator(&create_and_tt)},
        {"and3", make_tt_generator(&create_and3_tt)},
        {"and_fanout", make_tt_generator(&create_and_fan_out_tt)},
        {"and_or", make_tt_generator(&create_and_or_tt)},
        {"and_xor", make_tt_generator(&create_and_xor_tt)},
        {"b_fanout", make_tt_generator(&create_b_fan_out_tt)},
        {"crossing_wire", make_tt_generator(&create_crossing_wire_tt)},
        {"cx", make_tt_generator(&create_crossing_wire_tt)},
        {"demux_a_by_b", make_tt_generator(&create_demux_a_by_b_tt)},
        {"dot", make_tt_generator(&create_dot_tt)},
        {"double_wire", make_tt_generator(&create_double_wire_tt)},
        {"hourglass", make_tt_generator(&create_double_wire_tt)},
        {"fanout", make_tt_generator(&create_fan_out_tt)},
        {"false", make_tt_generator(&create_false_fan_out_tt)},
        {"false_fanout", make_tt_generator(&create_false_fan_out_tt)},
        {"fo2", make_tt_generator(&create_fan_out_tt)},
        {"ge", make_tt_generator(&create_ge_tt)},
        {"gt", make_tt_generator(&create_gt_tt)},
        {"gt_lt", make_tt_generator(&create_gt_lt_tt)},
        {"half_adder", make_tt_generator(&create_half_adder_tt)},
        {"ha", make_tt_generator(&create_half_adder_tt)},
        {"inv", make_tt_generator(&create_not_tt)},
        {"inv_diag", make_tt_generator(&create_not_tt)},
        {"ite", make_tt_generator(&create_ite_tt)},
        {"le", make_tt_generator(&create_le_tt)},
        {"lt", make_tt_generator(&create_lt_tt)},
        {"maj", make_tt_generator(&create_maj_tt)},
        {"majority", make_tt_generator(&create_maj_tt)},
        {"nand", make_tt_generator(&create_nand_tt)},
        {"nand_fanout", make_tt_generator(&create_nand_fan_out_tt)},
        {"nor", make_tt_generator(&create_nor_tt)},
        {"nor_fanout", make_tt_generator(&create_nor_fan_out_tt)},
        {"not", make_tt_generator(&create_not_tt)},
        {"not_a_and_b_fanout", make_tt_generator(&create_not_a_and_b_fan_out_tt)},
        {"not_a_fanout", make_tt_generator(&create_not_a_fan_out_tt)},
        {"not_b_fanout", make_tt_generator(&create_not_b_fan_out_tt)},
        {"not_a_or_b_fanout", make_tt_generator(&create_not_a_or_b_fan_out_tt)},
        {"onehot", make_tt_generator(&create_onehot_tt)},
        {"or", make_tt_generator(&create_or_tt)},
        {"or_fanout", make_tt_generator(&create_or_fan_out_tt)},
        {"or_and", make_tt_generator(&create_or_and_tt)},
        {"pass_left_and", make_tt_generator(&create_pass_left_and_tt)},
        {"pass_left_or", make_tt_generator(&create_pass_left_or_tt)},
        {"pass_left_xor", make_tt_generator(&create_pass_left_xor_tt)},
        {"true", make_tt_generator(&create_true_fan_out_tt)},
        {"true_fanout", make_tt_generator(&create_true_fan_out_tt)},
        {"wire", make_tt_generator(&create_id_tt)},
        {"wire_diag", make_tt_generator(&create_id_tt)},
        {"xor", make_tt_generator(&create_xor_tt)},
        {"xor3", make_tt_generator(&create_xor3_tt)},
        {"xor_and", make_tt_generator(&create_xor_and_tt)},
        {"xor_fanout", make_tt_generator(&create_xor_fan_out_tt)},
        {"xnor", make_tt_generator(&create_xnor_tt)},
        {"xnor_fanout", make_tt_generator(&create_xnor_fan_out_tt)}};

    return lookup;
}

std::optional<std::vector<tt>> resolve_truth_tables(const std::vector<std::string>& candidates)
{
    const auto& lookup = truth_table_lookup();

    const auto try_lookup = [&lookup](const std::string& candidate_name) -> const truth_table_generator* {
        if (const auto it = lookup.find(candidate_name); it != lookup.end())
        {
            return &it->second;
        }
        return nullptr;
    };

    for (const auto& name : candidates)
    {
        if (const auto* generator = try_lookup(name); generator != nullptr)
        {
            return (*generator)();
        }

        const auto fallback_names = candidate_gate_names(name);
        for (const auto& fallback : fallback_names)
        {
            if (fallback == name)
            {
                continue;
            }
            if (const auto* generator = try_lookup(fallback); generator != nullptr)
            {
                return (*generator)();
            }
        }
    }

    return std::nullopt;
}

struct layout_entry
{
    std::filesystem::path path;
    layout_t              layout;
};

void append_unique_candidates(std::vector<std::string>& target, const std::vector<std::string>& additions)
{
    for (const auto& candidate : additions)
    {
        if (std::find(target.begin(), target.end(), candidate) == target.end())
        {
            target.push_back(candidate);
        }
    }
}

std::string join_paths(const std::vector<std::filesystem::path>& paths)
{
    std::string result{};
    for (std::size_t idx = 0; idx < paths.size(); ++idx)
    {
        result.append(paths[idx].string());
        if (idx + 1 < paths.size())
        {
            result.append(", ");
        }
    }
    return result;
}

void print_usage(const char* executable)
{
    std::cout << "Usage: " << executable
              << " --input-dir <path> [--input-dir <path> ...] [--output-dir <path>] [--sample-count <count>] "
                 "[--verbose]\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    std::vector<std::filesystem::path> input_dirs{};
    std::filesystem::path              output_dir = std::filesystem::current_path();
    bool                               verbose    = false;
    std::optional<std::size_t>         sample_count{};

    if (argc == 1)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "--input-dir" || arg == "-i")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --input-dir requires a path argument.\n";
                return EXIT_FAILURE;
            }
            input_dirs.emplace_back(argv[++i]);
        }
        else if (arg == "--output-dir" || arg == "-o")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --output-dir requires a path argument.\n";
                return EXIT_FAILURE;
            }
            output_dir = argv[++i];
        }
        else if (arg == "--sample-count" || arg == "-s")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --sample-count requires an integer argument.\n";
                return EXIT_FAILURE;
            }
            const std::string count_arg = argv[++i];
            try
            {
                const auto value = std::stoull(count_arg);
                if (value == 0)
                {
                    std::cerr << "Error: --sample-count must be greater than zero.\n";
                    return EXIT_FAILURE;
                }
                sample_count = static_cast<std::size_t>(value);
            }
            catch (const std::exception&)
            {
                std::cerr << "Error: invalid numeric value for --sample-count: '" << count_arg << "'.\n";
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--verbose" || arg == "-v")
        {
            verbose = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        else
        {
            std::cerr << "Error: Unknown argument '" << arg << "'.\n";
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (input_dirs.empty())
    {
        std::cerr << "Error: --input-dir must be specified at least once.\n";
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    for (const auto& input_dir : input_dirs)
    {
        if (!std::filesystem::exists(input_dir) || !std::filesystem::is_directory(input_dir))
        {
            std::cerr << "Error: '" << input_dir.string() << "' is not a valid directory.\n";
            return EXIT_FAILURE;
        }
    }

    std::vector<std::filesystem::path> sqd_files{};
    for (const auto& input_dir : input_dirs)
    {
        for (const auto& entry : std::filesystem::directory_iterator(input_dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            if (entry.path().extension() == ".sqd")
            {
                sqd_files.push_back(entry.path());
            }
        }
    }

    if (sample_count.has_value() && !sqd_files.empty())
    {
        if (*sample_count >= sqd_files.size())
        {
            if (*sample_count > sqd_files.size())
            {
                std::cerr << "Warning: requested sample count (" << *sample_count << ") exceeds available SQD files ("
                          << sqd_files.size() << "). Processing all files without sampling.\n";
            }
        }
        else
        {
            std::random_device rd;
            std::mt19937       generator{rd()};
            std::shuffle(sqd_files.begin(), sqd_files.end(), generator);
            sqd_files.resize(*sample_count);
        }
    }

    std::sort(sqd_files.begin(), sqd_files.end());

    if (sqd_files.empty())
    {
        const auto dir_list = join_paths(input_dirs);
        std::cout << "No SQD files found in [" << dir_list << "]. Nothing to do.\n";
        return EXIT_SUCCESS;
    }

    std::unordered_map<std::string, std::vector<layout_entry>> grouped_layouts{};
    std::unordered_map<std::string, std::vector<std::string>>  group_candidates{};
    std::vector<std::string>                                   load_errors{};

    for (const auto& path : sqd_files)
    {
        const auto base_name  = path.stem().string();
        const auto candidates = candidate_gate_names(base_name);
        const auto group_key  = candidates.size() > 1 ? candidates[1] : candidates.front();

        try
        {
            auto layout = read_sqd_layout<layout_t>(path.string());
            grouped_layouts[group_key].push_back({path, std::move(layout)});
            append_unique_candidates(group_candidates[group_key], candidates);
        }
        catch (const std::exception& ex)
        {
            load_errors.push_back(fmt::format("{}: {}", path.string(), ex.what()));
        }
    }

    if (grouped_layouts.empty())
    {
        const auto dir_list = join_paths(input_dirs);
        std::cout << "No valid SQD layouts could be loaded from [" << dir_list << "].\n";
        if (!load_errors.empty())
        {
            std::cerr << "Encountered " << load_errors.size() << " error(s) while loading files.\n";
        }
        return load_errors.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (!output_dir.empty())
    {
        std::filesystem::create_directories(output_dir);
    }

    const auto table_path = output_dir / "fom_table.csv";
    const auto best_path  = output_dir / "fom_best_only.csv";

    std::ofstream table_file{table_path.string()};
    if (!table_file.is_open())
    {
        std::cerr << "Error: cannot open '" << table_path.string() << "' for writing.\n";
        return EXIT_FAILURE;
    }
    std::ofstream best_file{best_path.string()};
    if (!best_file.is_open())
    {
        std::cerr << "Error: cannot open '" << best_path.string() << "' for writing.\n";
        return EXIT_FAILURE;
    }

    table_file << "gate,file,index,critical_temperature_K,operational_domain_ratio,"
                  "defect_clearance_arsenic_nm,defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";
    best_file << "gate,file,index,critical_temperature_K,operational_domain_ratio,"
                 "defect_clearance_arsenic_nm,defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";

    std::vector<std::string> gate_names{};
    gate_names.reserve(grouped_layouts.size());
    for (const auto& [name, _] : grouped_layouts)
    {
        gate_names.push_back(name);
    }
    std::sort(gate_names.begin(), gate_names.end());

    const is_operational_params base_params{
        sidb_simulation_parameters{2, -0.32},
        sidb_simulation_engine::CLUSTERCOMPLETE,
        bdl_input_iterator_params{detect_bdl_wires_params{3.0}},
        is_operational_params::operational_condition::REJECT_KINKS};

    std::size_t total_processed = 0;

    for (const auto& gate_name : gate_names)
    {
        const auto candidate_it = group_candidates.find(gate_name);
        const auto& candidates =
            candidate_it != group_candidates.end() ? candidate_it->second : candidate_gate_names(gate_name);

        const auto truth_tables = resolve_truth_tables(candidates);
        if (!truth_tables.has_value())
        {
            std::cerr << "[FoM batch] Skipping gate '" << gate_name
                      << "' (no known truth table generator for candidates: ";
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                std::cerr << candidates[i];
                if (i + 1 < candidates.size())
                {
                    std::cerr << ", ";
                }
            }
            std::cerr << ").\n";
            continue;
        }

        const auto& group_entries = grouped_layouts.at(gate_name);

        std::vector<layout_t> layouts{};
        layouts.reserve(group_entries.size());
        for (const auto& entry : group_entries)
        {
            layouts.push_back(entry.layout);
        }

        const auto evaluation =
            fiction::experiments::evaluate_fom_metrics(layouts, *truth_tables, base_params, gate_name, verbose);
        if (!evaluation.has_value())
        {
            std::cerr << "[FoM batch] FoM evaluation failed for gate '" << gate_name << "'.\n";
            continue;
        }

        const auto& metrics = evaluation->metrics;

        for (std::size_t i = 0; i < metrics.size(); ++i)
        {
            const auto& entry     = metrics[i];
            const auto  file_name = group_entries[i].path.filename().string();

            table_file << fmt::format("{},{},{}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}\n", gate_name,
                                      file_name, i, entry.critical_temperature, entry.operational_domain_ratio,
                                      entry.defect_clearance_arsenic, entry.defect_clearance_vacancy,
                                      entry.band_bending_resilience_mV, entry.chi_value);

            if (i == evaluation->best_index)
            {
                best_file << fmt::format("{},{},{}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}\n", gate_name,
                                         file_name, i, entry.critical_temperature, entry.operational_domain_ratio,
                                         entry.defect_clearance_arsenic, entry.defect_clearance_vacancy,
                                         entry.band_bending_resilience_mV, entry.chi_value);
            }
        }

        ++total_processed;

        if (verbose)
        {
            fmt::print("[FoM batch] Gate '{}' processed ({} layout(s), best index {}, chi = {:.6f}).\n", gate_name,
                       metrics.size(), evaluation->best_index, evaluation->best_chi);
        }
    }

    if (verbose)
    {
        fmt::print("[FoM batch] Wrote FoM summaries to '{}' ({} gate group(s)).\n", output_dir.string(),
                   total_processed);
    }
    else
    {
        std::cout << "FoM evaluation complete. Reports written to '" << output_dir.string() << "'. Processed "
                  << total_processed << " gate group(s).\n";
    }

    if (!load_errors.empty())
    {
        std::cerr << "[FoM batch] Encountered " << load_errors.size()
                  << " error(s) while reading SQD files. Check the log above for details.\n";
    }

    return EXIT_SUCCESS;
}
