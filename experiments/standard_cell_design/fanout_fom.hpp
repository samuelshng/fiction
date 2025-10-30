#pragma once

#include "fiction_experiments.hpp"

#include <optional>
#include <string>
#include <vector>

#include <type_traits>

#include <iostream>

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

#include <fstream>
#include <limits>
#include <sstream>

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

/**
 * Computes figures of merit for a collection of SiDB gate layouts and, optionally, writes a report to disk.
 *
 * @tparam Lyt SiDB layout type.
 * @param gate_designs Layouts to analyze.
 * @param truth_tables Expected Boolean behaviour of the layouts.
 * @param base_params Operational parameters used during gate design. The same settings are re-used here to avoid
 *        redundant configuration.
 * @param gate_name Name used to label console output and saved reports.
 * @param save_report When true, a text report `<gate_name>_fom_report.txt` is written to the current directory.
 * @return Index of the layout with the smallest FoM cost (chi value). If the input is empty, `std::nullopt` is
 *         returned.
 */
template <typename Lyt>
[[nodiscard]] std::optional<std::size_t> compute_fom(const std::vector<Lyt>&           gate_designs,
                                                     const std::vector<tt>&            truth_tables,
                                                     const is_operational_params&      base_params,
                                                     const std::string&                gate_name,
                                                     const bool                        save_report)
{
    if (gate_designs.empty())
    {
        fmt::print("  [FoM] No layouts available for {}. Skipping FoM evaluation.\n", gate_name);
        if (save_report)
        {
            const auto filename = fmt::format("{}_fom_report.txt", gate_name);
            std::ofstream report_file{filename};
            if (!report_file.is_open())
            {
                fmt::print("  [FoM] Failed to write FoM report to '{}'\n", filename);
            }
            else
            {
                report_file
                    << "index,critical_temperature_K,operational_domain_ratio,defect_clearance_arsenic_nm,"
                       "defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";
                fmt::print("  [FoM] Empty report saved to '{}'\n", filename);
            }
        }
        return std::nullopt;
    }

    // Set up shared parameter structs, mirroring the experiment defaults to keep results comparable.
    critical_temperature_params ct_params{base_params};

    operational_domain_params op_domain_params{base_params};
    op_domain_params.sweep_dimensions = {{sweep_parameter::EPSILON_R}, {sweep_parameter::LAMBDA_TF}};
    op_domain_params.sweep_dimensions[0].min  = 4.0;
    op_domain_params.sweep_dimensions[0].max  = 6.0;
    op_domain_params.sweep_dimensions[0].step = 0.2;
    op_domain_params.sweep_dimensions[1].min  = 4.0;
    op_domain_params.sweep_dimensions[1].max  = 6.0;
    op_domain_params.sweep_dimensions[1].step = 0.2;

    band_bending_resilience_params bbr_params{
        physical_population_stability_params{base_params.simulation_parameters}};
    bbr_params.bdl_iterator_params = base_params.input_bdl_iterator_params;

    const auto si_vacancy = sidb_defect{sidb_defect_type::SI_VACANCY, -1, 10.6, 5.9};
    const auto arsenic    = sidb_defect{sidb_defect_type::ARSENIC, 1, 9.7, 2.1};

    const std::vector<sidb_defect> defects = {si_vacancy, arsenic};

    std::vector<fom_metrics> metrics{};
    metrics.reserve(gate_designs.size());

    double max_ct              = 0.0;
    double max_op_domain_ratio = 0.0;
    double max_clearance_ars   = 0.0;
    double max_clearance_vac   = 0.0;
    double max_bbr             = 0.0;

    const auto process_layout = [&](const auto& layout_for_fom)
    {
        using layout_type = std::decay_t<decltype(layout_for_fom)>;
        static_assert(has_cube_coord_v<layout_type>, "FoM computation expects cube coordinates");

        fom_metrics current{};

        operational_domain_stats op_stats{};
        const auto op_domain =
            operational_domain_grid_search(layout_for_fom, truth_tables, op_domain_params, &op_stats);
        static_cast<void>(op_domain);

        if (op_stats.num_total_parameter_points != 0U)
        {
            current.operational_domain_ratio = static_cast<double>(op_stats.num_operational_parameter_combinations) /
                                               static_cast<double>(op_stats.num_total_parameter_points);
        }

        current.critical_temperature = critical_temperature_gate_based(layout_for_fom, truth_tables, ct_params);

        const auto bbr_in_volt = band_bending_resilience(layout_for_fom, truth_tables, bbr_params);
        current.band_bending_resilience_mV = bbr_in_volt * 1000.0;

        defect_influence_params<cell<layout_type>> defect_params{};
        defect_params.additional_scanning_area = {20, 20};
        defect_params.operational_params       = base_params;

        for (const auto& defect : defects)
        {
            defect_params.defect = defect;

            defect_influence_stats defect_stats{};
            const auto             defect_grid =
                defect_influence_grid_search(layout_for_fom, truth_tables, defect_params, 4, &defect_stats);
            static_cast<void>(defect_grid);

            const auto clearance = calculate_defect_clearance(layout_for_fom, defect_grid);

            if (defect.type == sidb_defect_type::SI_VACANCY)
            {
                current.defect_clearance_vacancy = clearance.defect_clearance_distance;
            }
            else if (defect.type == sidb_defect_type::ARSENIC)
            {
                current.defect_clearance_arsenic = clearance.defect_clearance_distance;
            }
        }

        max_ct              = std::max(max_ct, current.critical_temperature);
        max_op_domain_ratio = std::max(max_op_domain_ratio, current.operational_domain_ratio);
        max_clearance_ars   = std::max(max_clearance_ars, current.defect_clearance_arsenic);
        max_clearance_vac   = std::max(max_clearance_vac, current.defect_clearance_vacancy);
        max_bbr             = std::max(max_bbr, current.band_bending_resilience_mV);

        metrics.push_back(current);
    };

    for (const auto& layout : gate_designs)
    {
        if constexpr (has_cube_coord_v<Lyt>)
        {
            process_layout(layout);
        }
        else
        {
            const auto layout_cube =
                convert_layout_to_fiction_coordinates<sidb_100_cell_clk_lyt_cube>(layout);
            process_layout(layout_cube);
        }
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

    double            best_chi  = std::numeric_limits<double>::max();
    std::size_t       best_idx  = 0U;
    std::ostringstream report_buffer{};

    report_buffer << "index,critical_temperature_K,operational_domain_ratio,defect_clearance_arsenic_nm,"
                     "defect_clearance_vacancy_nm,band_bending_resilience_mV,chi\n";

    fmt::print("  [FoM] Evaluating {} layout(s) for gate '{}'\n", metrics.size(), gate_name);

    for (std::size_t i = 0; i < metrics.size(); ++i)
    {
        auto& entry = metrics[i];

        const auto norm_ct   = safe_norm(entry.critical_temperature, max_ct);
        const auto norm_op   = safe_norm(entry.operational_domain_ratio, max_op_domain_ratio);
        const auto norm_ars  = safe_norm(entry.defect_clearance_arsenic, max_clearance_ars);
        const auto norm_vac  = safe_norm(entry.defect_clearance_vacancy, max_clearance_vac);
        const auto norm_bbr  = safe_norm(entry.band_bending_resilience_mV, max_bbr);
        entry.chi_value      = w_ct * norm_ct + w_op_domain * norm_op + w_defect_arsenic * norm_ars +
                          w_defect_vacancy * norm_vac + w_bbr * norm_bbr;

        if (entry.chi_value < best_chi)
        {
            best_chi = entry.chi_value;
            best_idx = i;
        }

        fmt::print("    [{}] CT={:.2f} K | OPD={:.3f} | MDC_As={:.3f} nm | MDC_Vac={:.3f} nm | "
                   "BBR={:.2f} mV | chi={:.3f}\n",
                   i, entry.critical_temperature, entry.operational_domain_ratio, entry.defect_clearance_arsenic,
                   entry.defect_clearance_vacancy, entry.band_bending_resilience_mV, entry.chi_value);

        report_buffer << fmt::format("{}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}, {:.6f}\n", i,
                                     entry.critical_temperature, entry.operational_domain_ratio,
                                     entry.defect_clearance_arsenic, entry.defect_clearance_vacancy,
                                     entry.band_bending_resilience_mV, entry.chi_value);
    }

    fmt::print("  [FoM] Best layout index for '{}' is {} (chi = {:.3f})\n", gate_name, best_idx, best_chi);

    if (save_report)
    {
        const auto filename = fmt::format("{}_fom_report.txt", gate_name);
        std::ofstream report_file{filename};
        if (!report_file.is_open())
        {
            fmt::print("  [FoM] Failed to write FoM report to '{}'\n", filename);
        }
        else
        {
            report_file << report_buffer.str();
            fmt::print("  [FoM] Report saved to '{}'\n", filename);
        }
    }

    return best_idx;
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
[[nodiscard]] std::optional<std::size_t> compute_fom(const std::vector<Lyt>&      gate_designs,
                                                     const std::vector<tt>&       truth_tables,
                                                     const is_operational_params& base_params,
                                                     const std::string&           gate_name,
                                                     const bool                   save_report)
{
    static_cast<void>(gate_designs);
    static_cast<void>(truth_tables);
    static_cast<void>(base_params);
    static_cast<void>(save_report);

    detail::notify_missing_alglib(gate_name);
    return std::nullopt;
}

#endif  // FICTION_ALGLIB_ENABLED

}  // namespace fiction::experiments
