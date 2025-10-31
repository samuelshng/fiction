#pragma once

#include "fiction_experiments.hpp"

#include <optional>
#include <string>
#include <vector>

#include <type_traits>

#include <iostream>
#include <filesystem>
#include <utility>

#if (FICTION_ALGLIB_ENABLED)

#include <fiction/algorithms/simulation/sidb/band_bending_resilience.hpp>
#include <fiction/algorithms/simulation/sidb/critical_temperature.hpp>
#include <fiction/algorithms/simulation/sidb/defect_clearance.hpp>
#include <fiction/algorithms/simulation/sidb/defect_influence.hpp>
#include <fiction/algorithms/simulation/sidb/operational_domain.hpp>
#include <fiction/algorithms/simulation/sidb/physical_population_stability.hpp>
#include <fiction/technology/sidb_defects.hpp>
#include <fiction/traits.hpp>
#include <fiction/types.hpp>
#include <fiction/utils/layout_utils.hpp>
#include <fiction/utils/math_utils.hpp>

#include <fmt/core.h>
#include <fmt/format.h>

#include <atomic>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <iomanip>

namespace fiction::experiments
{

/**
 * Small container for storing the FoM values of a single layout.
 */
struct fom_metrics
{
    double critical_temperature{0.0};
    double operational_domain_ratio{0.0};
    double defect_clearance_arsenic{0.0};
    double defect_clearance_vacancy{0.0};
    double band_bending_resilience_mV{0.0};
    double chi_value{0.0};
};

struct fom_evaluation_result
{
    std::vector<fom_metrics> metrics{};
    std::size_t              best_index{0U};
    double                   best_chi{std::numeric_limits<double>::max()};
};

/**
 * Evaluates figures of merit for a collection of SiDB gate layouts.
 *
 * @tparam Lyt SiDB layout type.
 * @param gate_designs Layouts to analyze.
 * @param truth_tables Expected Boolean behaviour of the layouts.
 * @param base_params Operational parameters used during gate design.
 * @param gate_name Name used to label console output.
 * @param verbose_console When true, progress information is printed to stdout.
 * @return Metrics for every layout alongside the index of the best layout. Returns `std::nullopt` if the input is
 *         empty.
 */
template <typename Lyt>
[[nodiscard]] std::optional<fom_evaluation_result> evaluate_fom_metrics(
    const std::vector<Lyt>&           gate_designs,
    const std::vector<tt>&            truth_tables,
    const is_operational_params&      base_params,
    const std::string&                gate_name,
    const bool                        verbose_console = true)
{
    if (gate_designs.empty())
    {
        if (verbose_console)
        {
            fmt::print("  [FoM] No layouts available for {}. Skipping FoM evaluation.\n", gate_name);
        }
        return std::nullopt;
    }

    const auto total_layouts = gate_designs.size();
    std::vector<fom_metrics> metrics(total_layouts);
    auto current_timestamp = []() -> std::string
    {
        using clock      = std::chrono::system_clock;
        const auto now   = clock::to_time_t(clock::now());
        std::tm tm_buf{};
#if defined(_MSC_VER)
        localtime_s(&tm_buf, &now);
#else
        localtime_r(&now, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    };

    if (verbose_console)
    {
        fmt::print("  [FoM] {} Evaluating {} layout(s) for gate '{}'\n", current_timestamp(), gate_designs.size(),
                   gate_name);
    }

    struct fom_thread_context
    {
        critical_temperature_params     ct_params;
        operational_domain_params       op_domain_params;
        band_bending_resilience_params  bbr_params;
        std::vector<sidb_defect>        defects;
    };

    const auto initialize_context = [&]() -> fom_thread_context
    {
        fom_thread_context ctx{critical_temperature_params{base_params}, operational_domain_params{base_params},
                               band_bending_resilience_params{
                                   physical_population_stability_params{base_params.simulation_parameters}}, {}};

        ctx.op_domain_params.sweep_dimensions = {{sweep_parameter::EPSILON_R}, {sweep_parameter::LAMBDA_TF}};
        ctx.op_domain_params.sweep_dimensions[0].min  = 4.0;
        ctx.op_domain_params.sweep_dimensions[0].max  = 6.0;
        ctx.op_domain_params.sweep_dimensions[0].step = 0.2;
        ctx.op_domain_params.sweep_dimensions[1].min  = 4.0;
        ctx.op_domain_params.sweep_dimensions[1].max  = 6.0;
        ctx.op_domain_params.sweep_dimensions[1].step = 0.2;

        ctx.bbr_params.bdl_iterator_params = base_params.input_bdl_iterator_params;

        ctx.defects = {
            sidb_defect{sidb_defect_type::SI_VACANCY, -1, 10.6, 5.9},
            sidb_defect{sidb_defect_type::ARSENIC, 1, 9.7, 2.1},
        };

        return ctx;
    };

    auto compute_with_context = [&](fom_thread_context& ctx, const auto& layout_for_fom, fom_metrics& entry)
    {
        using layout_type = std::decay_t<decltype(layout_for_fom)>;
        static_assert(has_cube_coord_v<layout_type>, "FoM computation expects cube coordinates");

        entry = {};

        operational_domain_stats op_stats{};
        const auto op_domain =
            operational_domain_grid_search(layout_for_fom, truth_tables, ctx.op_domain_params, &op_stats);
        static_cast<void>(op_domain);

        if (op_stats.num_total_parameter_points != 0U)
        {
            entry.operational_domain_ratio = static_cast<double>(op_stats.num_operational_parameter_combinations) /
                                             static_cast<double>(op_stats.num_total_parameter_points);
        }

        entry.critical_temperature = critical_temperature_gate_based(layout_for_fom, truth_tables, ctx.ct_params);

        const auto bbr_in_volt = band_bending_resilience(layout_for_fom, truth_tables, ctx.bbr_params);
        entry.band_bending_resilience_mV = bbr_in_volt * 1000.0;

        defect_influence_params<cell<layout_type>> defect_params{};
        defect_params.additional_scanning_area = {20, 20};
        defect_params.operational_params       = base_params;

        for (const auto& defect : ctx.defects)
        {
            defect_params.defect = defect;

            defect_influence_stats defect_stats{};
            const auto             defect_grid =
                defect_influence_grid_search(layout_for_fom, truth_tables, defect_params, 4, &defect_stats);
            static_cast<void>(defect_grid);

            const auto clearance = calculate_defect_clearance(layout_for_fom, defect_grid);

            if (defect.type == sidb_defect_type::SI_VACANCY)
            {
                entry.defect_clearance_vacancy = clearance.defect_clearance_distance;
            }
            else if (defect.type == sidb_defect_type::ARSENIC)
            {
                entry.defect_clearance_arsenic = clearance.defect_clearance_distance;
            }
        }
    };

    const auto compute_metrics_for_index = [&](const std::size_t idx, fom_thread_context& ctx, fom_metrics& entry)
    {
        if constexpr (has_cube_coord_v<Lyt>)
        {
            compute_with_context(ctx, gate_designs[idx], entry);
        }
        else
        {
            const auto layout_cube =
                convert_layout_to_fiction_coordinates<sidb_100_cell_clk_lyt_cube>(gate_designs[idx]);
            compute_with_context(ctx, layout_cube, entry);
        }
    };

    const auto hardware_concurrency = std::thread::hardware_concurrency();
    std::size_t hardware_threads = hardware_concurrency == 0U ? 1U : static_cast<std::size_t>(hardware_concurrency);
    std::size_t thread_cap       = hardware_threads;

    constexpr std::size_t default_cap = 128U;
    if (thread_cap > default_cap)
    {
        thread_cap = default_cap;
    }

    if (const char* env_value = std::getenv("FICTION_FOM_MAX_THREADS"))
    {
        const auto parsed = static_cast<std::size_t>(std::strtoull(env_value, nullptr, 10));
        if (parsed > 0U)
        {
            const auto clamped_env = std::max<std::size_t>(1U, std::min<std::size_t>(parsed, default_cap));
            thread_cap             = std::min<std::size_t>(thread_cap, clamped_env);
        }
    }

    thread_cap = std::max<std::size_t>(1U, std::min<std::size_t>(thread_cap, hardware_threads));

    const std::size_t thread_count = std::min<std::size_t>(thread_cap, total_layouts);

    if (verbose_console)
    {
        fmt::print("  [FoM] {} Using {} worker thread(s) (hardware {}, cap {}).\n", current_timestamp(),
                   thread_count, hardware_threads, thread_cap);
    }

    std::vector<fom_thread_context> contexts(thread_count);
    for (auto& ctx : contexts)
    {
        ctx = initialize_context();
    }

    if (thread_count <= 1)
    {
        auto& ctx = contexts.front();
        for (std::size_t idx = 0; idx < total_layouts; ++idx)
        {
            compute_metrics_for_index(idx, ctx, metrics[idx]);
            if (verbose_console)
            {
                const auto finished = idx + 1U;
                const auto& entry   = metrics[idx];
                fmt::print("    [FoM progress] {} ({}/{}) idx={} | CT={:.2f} K | OPD={:.3f} | "
                           "MDC_As={:.3f} nm | MDC_Vac={:.3f} nm | BBR={:.2f} mV\n",
                           current_timestamp(), finished, total_layouts, idx, entry.critical_temperature,
                           entry.operational_domain_ratio, entry.defect_clearance_arsenic,
                           entry.defect_clearance_vacancy, entry.band_bending_resilience_mV);
            }
        }
    }
    else
    {
        std::atomic_size_t         next_index{0};
        std::vector<std::thread>   workers{};
        std::mutex                 progress_mutex{};
        std::condition_variable    progress_cv{};
        std::queue<std::size_t>    progress_queue{};
        auto notify_progress = [&](const std::size_t idx)
        {
            if (!verbose_console)
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(progress_mutex);
                progress_queue.push(idx);
            }
            progress_cv.notify_one();
        };

        workers.reserve(thread_count);
        for (std::size_t t = 0; t < thread_count; ++t)
        {
            workers.emplace_back([&, t]()
            {
                auto& ctx = contexts[t];
                while (true)
                {
                    const auto idx = next_index.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= total_layouts)
                    {
                        break;
                    }

                    compute_metrics_for_index(idx, ctx, metrics[idx]);
                    notify_progress(idx);
                }
            });
        }

        if (verbose_console)
        {
            std::size_t printed = 0;
            std::unique_lock<std::mutex> lock(progress_mutex);
            while (printed < total_layouts)
            {
                progress_cv.wait(lock, [&]() { return !progress_queue.empty(); });
                while (!progress_queue.empty())
                {
                    const auto idx = progress_queue.front();
                    progress_queue.pop();
                    ++printed;
                    const auto entry = metrics[idx];
                    lock.unlock();
                    fmt::print("    [FoM progress] {} ({}/{}) idx={} | CT={:.2f} K | OPD={:.3f} | "
                               "MDC_As={:.3f} nm | MDC_Vac={:.3f} nm | BBR={:.2f} mV\n",
                               current_timestamp(), printed, total_layouts, idx, entry.critical_temperature,
                               entry.operational_domain_ratio, entry.defect_clearance_arsenic,
                               entry.defect_clearance_vacancy, entry.band_bending_resilience_mV);
                    lock.lock();
                }
            }
            lock.unlock();
        }

        for (auto& worker : workers)
        {
            worker.join();
        }
    }

    double max_ct              = 0.0;
    double max_op_domain_ratio = 0.0;
    double max_clearance_ars   = 0.0;
    double max_clearance_vac   = 0.0;
    double max_bbr             = 0.0;

    for (const auto& entry : metrics)
    {
        max_ct              = std::max(max_ct, entry.critical_temperature);
        max_op_domain_ratio = std::max(max_op_domain_ratio, entry.operational_domain_ratio);
        max_clearance_ars   = std::max(max_clearance_ars, entry.defect_clearance_arsenic);
        max_clearance_vac   = std::max(max_clearance_vac, entry.defect_clearance_vacancy);
        max_bbr             = std::max(max_bbr, entry.band_bending_resilience_mV);
    }

    const auto safe_norm = [](const double value, const double max_value) noexcept -> double
    {
        if (max_value <= std::numeric_limits<double>::epsilon())
        {
            return 0.0;
        }
        return value / max_value;
    };

    const auto w_ct             = -1.0;
    const auto w_op_domain      = -1.0;
    const auto w_defect_arsenic = 1.0;
    const auto w_defect_vacancy = 1.0;
    const auto w_bbr            = -1.0;

    double best_chi = std::numeric_limits<double>::max();
    std::size_t best_idx = 0U;

    for (std::size_t i = 0; i < metrics.size(); ++i)
    {
        auto& entry = metrics[i];

        const auto norm_ct  = safe_norm(entry.critical_temperature, max_ct);
        const auto norm_op  = safe_norm(entry.operational_domain_ratio, max_op_domain_ratio);
        const auto norm_ars = safe_norm(entry.defect_clearance_arsenic, max_clearance_ars);
        const auto norm_vac = safe_norm(entry.defect_clearance_vacancy, max_clearance_vac);
        const auto norm_bbr = safe_norm(entry.band_bending_resilience_mV, max_bbr);

        entry.chi_value =
            w_ct * norm_ct + w_op_domain * norm_op + w_defect_arsenic * norm_ars + w_defect_vacancy * norm_vac +
            w_bbr * norm_bbr;

        if (entry.chi_value < best_chi)
        {
            best_chi = entry.chi_value;
            best_idx = i;
        }

        if (verbose_console)
        {
            fmt::print("    [{}] CT={:.2f} K | OPD={:.3f} | MDC_As={:.3f} nm | MDC_Vac={:.3f} nm | "
                       "BBR={:.2f} mV | chi={:.3f}\n",
                       i, entry.critical_temperature, entry.operational_domain_ratio, entry.defect_clearance_arsenic,
                       entry.defect_clearance_vacancy, entry.band_bending_resilience_mV, entry.chi_value);
        }
    }

    fom_evaluation_result result{};
    result.metrics    = std::move(metrics);
    result.best_index = best_idx;
    result.best_chi   = best_chi;

    if (verbose_console)
    {
        fmt::print("  [FoM] Best layout index for '{}' is {} (chi = {:.3f})\n", gate_name, result.best_index,
                   result.best_chi);
        fmt::print("  [FoM] Completed FoM evaluation for '{}'.\n", gate_name);
    }

    return result;
}

/**
 * Computes figures of merit for a collection of SiDB gate layouts and, optionally, writes reports to disk.
 *
 * @tparam Lyt SiDB layout type.
 * @param gate_designs Layouts to analyze.
 * @param truth_tables Expected Boolean behaviour of the layouts.
 * @param base_params Operational parameters used during gate design. The same settings are re-used here to avoid
 *        redundant configuration.
 * @param gate_name Name used to label console output and saved reports.
 * @param save_report When true, a text report `<gate_name>_fom_report.txt` is written to the specified directory.
 * @param output_directory Directory in which FoM reports should be written when `save_report` is true.
 * @return Index of the layout with the smallest FoM cost (chi value). If no layouts are provided, `std::nullopt`
 *         is returned.
 */
template <typename Lyt>
[[nodiscard]] std::optional<std::size_t> compute_fom(const std::vector<Lyt>&           gate_designs,
                                                     const std::vector<tt>&            truth_tables,
                                                     const is_operational_params&      base_params,
                                                     const std::string&                gate_name,
                                                     const bool                        save_report,
                                                     const std::filesystem::path&      output_directory =
                                                         std::filesystem::current_path())
{
    if (gate_designs.empty())
    {
        fmt::print("  [FoM] No layouts available for {}. Skipping FoM evaluation.\n", gate_name);
        if (save_report)
        {
            const auto filepath = output_directory / fmt::format("{}_fom_report.txt", gate_name);
            const auto parent   = filepath.parent_path();
            if (!parent.empty())
            {
                std::filesystem::create_directories(parent);
            }
            std::ofstream report_file{filepath.string()};
            if (!report_file.is_open())
            {
                fmt::print("  [FoM] Failed to write FoM report to '{}'\n", filepath.string());
            }
            else
            {
                report_file
                    << "index,critical_temperature_K,operational_domain_ratio,defect_clearance_arsenic_nm,"
                       "defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";
                fmt::print("  [FoM] Empty report saved to '{}'\n", filepath.string());
            }

            const auto best_filepath = output_directory / fmt::format("{}_best_fom.txt", gate_name);
            const auto best_parent   = best_filepath.parent_path();
            if (!best_parent.empty())
            {
                std::filesystem::create_directories(best_parent);
            }
            std::ofstream best_report{best_filepath.string()};
            if (!best_report.is_open())
            {
                fmt::print("  [FoM] Failed to write best FoM report to '{}'\n", best_filepath.string());
            }
            else
            {
                best_report
                    << "index,critical_temperature_K,operational_domain_ratio,defect_clearance_arsenic_nm,"
                       "defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";
                fmt::print("  [FoM] Empty best-design report saved to '{}'\n", best_filepath.string());
            }
        }
        return std::nullopt;
    }

    const auto evaluation =
        evaluate_fom_metrics(gate_designs, truth_tables, base_params, gate_name, /*verbose_console=*/true);
    if (!evaluation.has_value())
    {
        return std::nullopt;
    }

    if (save_report)
    {
        const auto filepath = output_directory / fmt::format("{}_fom_report.txt", gate_name);
        const auto parent   = filepath.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }
        std::ofstream report_file{filepath.string()};
        if (!report_file.is_open())
        {
            fmt::print("  [FoM] Failed to write FoM report to '{}'\n", filepath.string());
        }
        else
        {
            report_file
                << "index,critical_temperature_K,operational_domain_ratio,defect_clearance_arsenic_nm,"
                   "defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";
            for (std::size_t i = 0; i < evaluation->metrics.size(); ++i)
            {
                const auto& entry = evaluation->metrics[i];
                report_file << fmt::format("{}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}\n", i,
                                           entry.critical_temperature, entry.operational_domain_ratio,
                                           entry.defect_clearance_arsenic, entry.defect_clearance_vacancy,
                                           entry.band_bending_resilience_mV, entry.chi_value);
            }
            fmt::print("  [FoM] Report saved to '{}'\n", filepath.string());
        }

        const auto best_filepath = output_directory / fmt::format("{}_best_fom.txt", gate_name);
        const auto best_parent   = best_filepath.parent_path();
        if (!best_parent.empty())
        {
            std::filesystem::create_directories(best_parent);
        }
        std::ofstream best_report{best_filepath.string()};
        if (!best_report.is_open())
        {
            fmt::print("  [FoM] Failed to write best FoM report to '{}'\n", best_filepath.string());
        }
        else
        {
            const auto& best_entry = evaluation->metrics[evaluation->best_index];
            best_report << "index,critical_temperature_K,operational_domain_ratio,defect_clearance_arsenic_nm,"
                           "defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";
            best_report << fmt::format("{}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}\n",
                                       evaluation->best_index, best_entry.critical_temperature,
                                       best_entry.operational_domain_ratio, best_entry.defect_clearance_arsenic,
                                       best_entry.defect_clearance_vacancy, best_entry.band_bending_resilience_mV,
                                       best_entry.chi_value);
            fmt::print("  [FoM] Best design metrics saved to '{}'\n", best_filepath.string());
        }
    }

    return evaluation->best_index;
}

#else  // FICTION_ALGLIB_ENABLED

namespace detail
{
inline void notify_missing_alglib(const std::string& gate_name)
{
    std::cerr << "[FoM] FoM analysis for '" << gate_name
              << "' skipped. Enable ALGLIB support to compute advanced metrics.\n";
}
}  // namespace detail

template <typename Lyt>
[[nodiscard]] std::optional<fom_evaluation_result> evaluate_fom_metrics(
    const std::vector<Lyt>&           gate_designs,
    const std::vector<tt>&            truth_tables,
    const is_operational_params&      base_params,
    const std::string&                gate_name,
    const bool                        verbose_console = true)
{
    static_cast<void>(gate_designs);
    static_cast<void>(truth_tables);
    static_cast<void>(base_params);
    static_cast<void>(verbose_console);

    detail::notify_missing_alglib(gate_name);
    return std::nullopt;
}

template <typename Lyt>
[[nodiscard]] std::optional<std::size_t> compute_fom(const std::vector<Lyt>&      gate_designs,
                                                     const std::vector<tt>&       truth_tables,
                                                     const is_operational_params& base_params,
                                                     const std::string&           gate_name,
                                                     const bool                   save_report,
                                                     const std::filesystem::path& output_directory =
                                                         std::filesystem::current_path())
{
    static_cast<void>(gate_designs);
    static_cast<void>(truth_tables);
    static_cast<void>(base_params);
    static_cast<void>(save_report);
    static_cast<void>(output_directory);

    detail::notify_missing_alglib(gate_name);
    return std::nullopt;
}

#endif  // FICTION_ALGLIB_ENABLED

}  // namespace fiction::experiments
