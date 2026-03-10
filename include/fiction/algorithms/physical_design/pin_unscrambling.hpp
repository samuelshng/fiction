/**
 * @file pin_unscrambling.hpp
 * @brief Pin unscrambling workflow for ROW-clocked hexagonal gate layouts.
 */

#ifndef FICTION_PIN_UNSCRAMBLING_HPP
#define FICTION_PIN_UNSCRAMBLING_HPP

#include "fiction/algorithms/physical_design/unscramble_pins.hpp"
#include "fiction/io/pin_unscrambling_spec.hpp"
#include "fiction/types.hpp"

#include <mockturtle/utils/stopwatch.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fiction
{

/**
 * @brief Runtime configuration for pin unscrambling.
 */
struct pin_unscrambling_configuration
{
    /**
     * @brief Desired semantic PI order.
     */
    std::vector<std::string> input_order{};
    /**
     * @brief Optional alias-to-semantic PI renaming list.
     */
    std::vector<pin_alias_semantic_mapping> input_mappings{};
    /**
     * @brief Desired semantic PO order.
     */
    std::vector<std::string> output_order{};
    /**
     * @brief Optional alias-to-semantic PO renaming list.
     */
    std::vector<pin_alias_semantic_mapping> output_mappings{};
    /**
     * @brief Enforce full order vectors to match PI/PO counts exactly.
     */
    bool strict_full_order{true};
    /**
     * @brief Optional JSON report output path.
     */
    std::optional<std::string> report_file{};
};

/**
 * @brief One resolved semantic-to-layout mapping entry.
 */
struct pin_unscrambling_mapping_entry
{
    /**
     * @brief Semantic pin name.
     */
    std::string semantic_name{};
    /**
     * @brief Alias name currently used by the source layout at this index.
     */
    std::string fgl_alias{};
};

/**
 * @brief Result summary for one pin unscrambling run.
 */
struct pin_unscrambling_report
{
    /**
     * @brief Topology string for the source layout.
     */
    std::string topology{};
    /**
     * @brief Number of primary inputs in the source layout.
     */
    uint32_t num_pis{};
    /**
     * @brief Number of primary outputs in the source layout.
     */
    uint32_t num_pos{};
    /**
     * @brief Resolved PI mapping in routed target order.
     */
    std::vector<pin_unscrambling_mapping_entry> input_mappings{};
    /**
     * @brief Resolved PO mapping in routed target order.
     */
    std::vector<pin_unscrambling_mapping_entry> output_mappings{};
    /**
     * @brief Total runtime in seconds.
     */
    double total_runtime_seconds{};
};

/**
 * @brief Typed output bundle of a pin-unscrambling run.
 *
 * @tparam Lyt Gate-level layout type.
 */
template <typename Lyt>
struct pin_unscrambling_result
{
    /**
     * @brief Unscrambled layout.
     */
    Lyt layout{};
    /**
     * @brief Run report.
     */
    pin_unscrambling_report report{};
};

namespace detail
{

/**
 * @brief Validates uniqueness and non-emptiness of resolved semantic names.
 *
 * @param names Ordered semantic name vector.
 * @param pin_kind Pin kind string used in diagnostics.
 */
inline void validate_resolved_semantic_names(const std::vector<std::string>& names, const std::string_view& pin_kind)
{
    std::unordered_set<std::string> seen{};
    seen.reserve(names.size());

    for (const auto& name : names)
    {
        if (name.empty())
        {
            throw std::invalid_argument(std::string{"Resolved "} + std::string{pin_kind} +
                                        " semantic names contain an empty entry.");
        }

        if (!seen.insert(name).second)
        {
            throw std::invalid_argument(std::string{"Resolved "} + std::string{pin_kind} +
                                        " semantic names must be unique. Duplicate entry: '" + name + "'.");
        }
    }
}

/**
 * @brief Builds a map from pin names to indices.
 *
 * @param names Ordered names.
 * @return Name-to-index map.
 */
inline std::unordered_map<std::string, uint32_t> build_name_to_index(const std::vector<std::string>& names)
{
    std::unordered_map<std::string, uint32_t> name_to_index{};
    name_to_index.reserve(names.size());

    for (uint32_t i = 0u; i < names.size(); ++i)
    {
        name_to_index.emplace(names[i], i);
    }

    return name_to_index;
}

/**
 * @brief Resolves desired semantic ordering to indices within the resolved semantic namespace.
 *
 * @param desired_order Desired semantic names.
 * @param resolved_names Ordered resolved semantic names.
 * @param strict_full_order Whether non-empty desired order must be complete.
 * @param pin_kind Pin kind string used in diagnostics.
 * @return Ordered semantic namespace indices.
 */
inline std::vector<uint32_t> resolve_order_indices(const std::vector<std::string>& desired_order,
                                                   const std::vector<std::string>& resolved_names,
                                                   const bool strict_full_order, const std::string_view& pin_kind)
{
    const auto name_to_index = build_name_to_index(resolved_names);

    if (desired_order.empty())
    {
        std::vector<uint32_t> identity_order(resolved_names.size(), 0u);
        for (uint32_t i = 0u; i < resolved_names.size(); ++i)
        {
            identity_order[i] = i;
        }

        return identity_order;
    }

    if (strict_full_order && desired_order.size() != resolved_names.size())
    {
        throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " order size (" +
                                    std::to_string(desired_order.size()) + ") does not match layout count (" +
                                    std::to_string(resolved_names.size()) + ").");
    }

    std::unordered_set<uint32_t> used_indices{};
    used_indices.reserve(resolved_names.size());

    std::vector<uint32_t> resolved_indices{};
    resolved_indices.reserve(resolved_names.size());

    for (const auto& desired_name : desired_order)
    {
        if (desired_name.empty())
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} +
                                        " order contains an empty entry.");
        }

        const auto semantic_it = name_to_index.find(desired_name);
        if (semantic_it == name_to_index.cend())
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " name '" + desired_name +
                                        "' is not present in the resolved semantic names.");
        }

        const auto semantic_index = semantic_it->second;
        if (!used_indices.insert(semantic_index).second)
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " name '" + desired_name +
                                        "' appears multiple times.");
        }

        resolved_indices.push_back(semantic_index);
    }

    if (!strict_full_order)
    {
        for (uint32_t i = 0u; i < resolved_names.size(); ++i)
        {
            if (used_indices.find(i) == used_indices.cend())
            {
                resolved_indices.push_back(i);
            }
        }
    }

    return resolved_indices;
}

/**
 * @brief Resolves semantic names for the current layout aliases.
 *
 * @param mappings Alias-to-semantic mappings.
 * @param current_aliases Ordered current aliases.
 * @param pin_kind Pin kind string used in diagnostics.
 * @return Resolved semantic names aligned with current layout order.
 */
inline std::vector<std::string> resolve_semantic_names(const std::vector<pin_alias_semantic_mapping>& mappings,
                                                       const std::vector<std::string>&                current_aliases,
                                                       const std::string_view&                        pin_kind)
{
    const auto alias_to_index = build_name_to_index(current_aliases);

    std::vector<std::string>     resolved = current_aliases;
    std::unordered_set<uint32_t> used_indices{};
    used_indices.reserve(current_aliases.size());

    for (const auto& mapping : mappings)
    {
        if (mapping.fgl_alias.empty())
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} +
                                        " mappings contain an empty 'fgl_alias'.");
        }

        if (mapping.semantic_name.empty())
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} +
                                        " mappings contain an empty 'semantic_name'.");
        }

        const auto alias_it = alias_to_index.find(mapping.fgl_alias);
        if (alias_it == alias_to_index.cend())
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " alias '" +
                                        mapping.fgl_alias + "' is not present in the current layout.");
        }

        const auto index = alias_it->second;
        if (!used_indices.insert(index).second)
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " alias '" +
                                        mapping.fgl_alias + "' appears multiple times.");
        }

        resolved[index] = mapping.semantic_name;
    }

    validate_resolved_semantic_names(resolved, pin_kind);

    return resolved;
}

/**
 * @brief Serializes and writes the run report JSON to disk.
 *
 * @param report Report payload.
 * @param filename Target JSON filename.
 */
inline void write_report_file(const pin_unscrambling_report& report, const std::string_view& filename)
{
    nlohmann::json report_json{};

    report_json["topology"]              = report.topology;
    report_json["num_pis"]               = report.num_pis;
    report_json["num_pos"]               = report.num_pos;
    report_json["total_runtime_seconds"] = report.total_runtime_seconds;

    report_json["input_mappings"]  = nlohmann::json::array();
    report_json["output_mappings"] = nlohmann::json::array();

    for (const auto& mapping : report.input_mappings)
    {
        report_json["input_mappings"].push_back(
            {{"semantic_name", mapping.semantic_name}, {"fgl_alias", mapping.fgl_alias}});
    }

    for (const auto& mapping : report.output_mappings)
    {
        report_json["output_mappings"].push_back(
            {{"semantic_name", mapping.semantic_name}, {"fgl_alias", mapping.fgl_alias}});
    }

    std::ofstream os{filename.data(), std::ofstream::out | std::ofstream::trunc};
    if (!os.is_open())
    {
        throw std::invalid_argument("Unable to open report file for writing: '" + std::string{filename} + "'.");
    }

    os << report_json.dump(2) << "\n";
    os.close();
}

/**
 * @brief Returns topology label for a supported layout type.
 *
 * @tparam Lyt Gate-level layout type.
 * @return Topology label.
 */
template <typename Lyt>
[[nodiscard]] inline std::string topology_label()
{
    if constexpr (std::is_same_v<Lyt, hex_even_row_gate_clk_lyt>)
    {
        return "even_row_hex";
    }

    if constexpr (std::is_same_v<Lyt, hex_odd_row_gate_clk_lyt>)
    {
        return "odd_row_hex";
    }

    return "unsupported";
}

}  // namespace detail

/**
 * @brief Executes one pin unscrambling run on a gate-level layout object.
 *
 * @tparam Lyt Gate-level layout type.
 * @param layout Source layout.
 * @param config Runtime configuration.
 * @return Unscrambled layout and report.
 */
template <typename Lyt>
[[nodiscard]] pin_unscrambling_result<Lyt> run_pin_unscrambling(const Lyt&                            layout,
                                                                const pin_unscrambling_configuration& config)
{
    static_assert(std::is_same_v<Lyt, hex_even_row_gate_clk_lyt> || std::is_same_v<Lyt, hex_odd_row_gate_clk_lyt>,
                  "run_pin_unscrambling currently supports only even_row_hex and odd_row_hex layouts.");

    if (!layout.is_clocking_scheme("ROW"))
    {
        throw std::invalid_argument("Input layout is not ROW-clocked, which is required by unscramble_pins.");
    }

    const auto use_input_order  = !config.input_order.empty();
    const auto use_output_order = !config.output_order.empty();

    if (!use_input_order)
    {
        throw std::invalid_argument("No PI order provided. Specify `input_order` via CLI and/or spec.");
    }

    if (!use_output_order)
    {
        throw std::invalid_argument("No PO order provided. Specify `output_order` via CLI and/or spec.");
    }

    std::vector<mockturtle::node<Lyt>> current_pis{};
    current_pis.reserve(layout.num_pis());

    std::vector<std::string> current_pi_aliases{};
    current_pi_aliases.reserve(layout.num_pis());

    layout.foreach_pi(
        [&layout, &current_pis, &current_pi_aliases](const auto& pi)
        {
            current_pis.push_back(pi);
            current_pi_aliases.push_back(layout.get_name(pi));
        });

    std::vector<mockturtle::signal<Lyt>> current_pos{};
    current_pos.reserve(layout.num_pos());

    std::vector<std::string> current_po_aliases{};
    current_po_aliases.reserve(layout.num_pos());

    uint32_t po_index = 0u;
    layout.foreach_po(
        [&layout, &current_pos, &current_po_aliases, &po_index](const auto& po)
        {
            current_pos.push_back(po);
            current_po_aliases.push_back(layout.get_output_name(po_index++));
        });

    std::vector<mockturtle::node<Lyt>>   target_pis{};
    std::vector<mockturtle::signal<Lyt>> target_pos{};

    pin_unscrambling_result<Lyt> result{};
    result.report.topology = detail::topology_label<Lyt>();
    result.report.num_pis  = static_cast<uint32_t>(current_pis.size());
    result.report.num_pos  = static_cast<uint32_t>(current_pos.size());

    const auto resolved_input_names  = detail::resolve_semantic_names(config.input_mappings, current_pi_aliases, "PI");
    const auto resolved_output_names = detail::resolve_semantic_names(config.output_mappings, current_po_aliases, "PO");

    const auto pi_target_indices =
        detail::resolve_order_indices(config.input_order, resolved_input_names, config.strict_full_order, "PI");
    target_pis.reserve(pi_target_indices.size());
    result.report.input_mappings.reserve(pi_target_indices.size());

    for (const auto index : pi_target_indices)
    {
        target_pis.push_back(current_pis[index]);
        result.report.input_mappings.push_back({resolved_input_names[index], current_pi_aliases[index]});
    }

    const auto po_target_indices =
        detail::resolve_order_indices(config.output_order, resolved_output_names, config.strict_full_order, "PO");
    target_pos.reserve(po_target_indices.size());
    result.report.output_mappings.reserve(po_target_indices.size());

    for (const auto index : po_target_indices)
    {
        target_pos.push_back(current_pos[index]);
        result.report.output_mappings.push_back({resolved_output_names[index], current_po_aliases[index]});
    }

    unscramble_pins_stats stats{};
    result.layout                       = unscramble_pins(layout, target_pis, target_pos, {}, &stats);
    result.report.total_runtime_seconds = mockturtle::to_seconds(stats.time_total);

    if (config.report_file.has_value())
    {
        detail::write_report_file(result.report, *config.report_file);
    }

    return result;
}

}  // namespace fiction

#endif  // FICTION_PIN_UNSCRAMBLING_HPP
