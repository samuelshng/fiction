/**
 * @file pin_unscrambling.hpp
 * @brief Pin unscrambling workflow for ROW-clocked hexagonal gate layouts.
 */

#ifndef FICTION_PIN_UNSCRAMBLING_HPP
#define FICTION_PIN_UNSCRAMBLING_HPP

#include "fiction/algorithms/physical_design/unscramble_pins.hpp"
#include "fiction/io/network_reader.hpp"
#include "fiction/io/pin_unscrambling_spec.hpp"
#include "fiction/types.hpp"

#include <mockturtle/utils/stopwatch.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
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
     * @brief Optional AIG file path used as canonical semantic I/O naming source.
     */
    std::optional<std::string> aig_file{};
    /**
     * @brief Desired semantic PI order.
     */
    std::vector<std::string> input_order{};
    /**
     * @brief Optional alias-to-semantic PI mapping list.
     */
    std::vector<pin_alias_semantic_mapping> input_mappings{};
    /**
     * @brief Desired semantic PO order.
     */
    std::vector<std::string> output_order{};
    /**
     * @brief Optional alias-to-semantic PO mapping list.
     */
    std::vector<pin_alias_semantic_mapping> output_mappings{};
    /**
     * @brief Enforce full order/mapping vectors to match PI/PO counts exactly.
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
     * @brief Canonical AIG declaration index, if available.
     */
    uint32_t canonical_index{};
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
 * @brief Sentinel canonical index used when no AIG reference exists.
 */
static constexpr uint32_t unknown_canonical_index = std::numeric_limits<uint32_t>::max();

/**
 * @brief Validates uniqueness and non-emptiness of canonical names.
 *
 * @param names Ordered name vector.
 * @param pin_kind Pin kind string used in diagnostics.
 */
inline void validate_canonical_names(const std::vector<std::string>& names, const std::string_view& pin_kind)
{
    std::unordered_set<std::string> seen{};
    seen.reserve(names.size());

    for (const auto& name : names)
    {
        if (name.empty())
        {
            throw std::invalid_argument(std::string{"Canonical "} + std::string{pin_kind} +
                                        " names contain an empty entry.");
        }

        if (!seen.insert(name).second)
        {
            throw std::invalid_argument(std::string{"Canonical "} + std::string{pin_kind} +
                                        " names must be unique. Duplicate entry: '" + name + "'.");
        }
    }
}

/**
 * @brief Extracts ordered semantic PI/PO names from a single AIG file.
 *
 * @param aig_file AIG file path.
 * @return Pair of ordered PI names and ordered PO names.
 */
inline std::pair<std::vector<std::string>, std::vector<std::string>>
extract_semantic_names_from_aig(const std::string_view& aig_file)
{
    std::stringstream       aig_reader_log{};
    network_reader<aig_ptr> reader{aig_file, aig_reader_log};

    const auto& networks = reader.get_networks(false);

    if (networks.empty())
    {
        throw std::invalid_argument("Unable to read AIG file or no network was parsed: '" + std::string{aig_file} +
                                    "'. Reader log: " + aig_reader_log.str());
    }

    if (networks.size() > 1u)
    {
        throw std::invalid_argument("AIG reader returned multiple networks. Provide exactly one AIG file.");
    }

    const auto& ntk = *networks.front();

    std::vector<std::string> input_names(ntk.num_pis());
    ntk.foreach_pi(
        [&ntk, &input_names](const auto& pi, const auto i)
        {
            const auto pi_signal = ntk.make_signal(pi);

            if (!ntk.has_name(pi_signal))
            {
                throw std::invalid_argument("AIG primary inputs must be named for semantic pin mapping.");
            }

            input_names[i] = ntk.get_name(pi_signal);
        });

    std::vector<std::string> output_names(ntk.num_pos());
    for (uint32_t i = 0u; i < ntk.num_pos(); ++i)
    {
        if (!ntk.has_output_name(i))
        {
            throw std::invalid_argument("AIG primary outputs must be named for semantic pin mapping.");
        }

        output_names[i] = ntk.get_output_name(i);
    }

    validate_canonical_names(input_names, "PI");
    validate_canonical_names(output_names, "PO");

    return {std::move(input_names), std::move(output_names)};
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
 * @brief Resolves desired semantic ordering to canonical declaration indices.
 *
 * @param desired_order Desired semantic names.
 * @param canonical_names Ordered canonical names.
 * @param strict_full_order Whether non-empty desired order must be complete.
 * @param pin_kind Pin kind string used in diagnostics.
 * @return Ordered canonical indices.
 */
inline std::vector<uint32_t> resolve_order_indices(const std::vector<std::string>& desired_order,
                                                   const std::vector<std::string>& canonical_names,
                                                   const bool strict_full_order, const std::string_view& pin_kind)
{
    const auto name_to_index = build_name_to_index(canonical_names);

    if (desired_order.empty())
    {
        std::vector<uint32_t> identity_order(canonical_names.size(), 0u);
        for (uint32_t i = 0u; i < canonical_names.size(); ++i)
        {
            identity_order[i] = i;
        }

        return identity_order;
    }

    if (strict_full_order && desired_order.size() != canonical_names.size())
    {
        throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " order size (" +
                                    std::to_string(desired_order.size()) + ") does not match canonical count (" +
                                    std::to_string(canonical_names.size()) + ").");
    }

    std::unordered_set<uint32_t> used_indices{};
    used_indices.reserve(canonical_names.size());

    std::vector<uint32_t> resolved_indices{};
    resolved_indices.reserve(canonical_names.size());

    for (const auto& desired_name : desired_order)
    {
        if (desired_name.empty())
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} +
                                        " order contains an empty entry.");
        }

        const auto canonical_it = name_to_index.find(desired_name);
        if (canonical_it == name_to_index.cend())
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " name '" + desired_name +
                                        "' is not present in the canonical AIG names.");
        }

        const auto canonical_index = canonical_it->second;
        if (!used_indices.insert(canonical_index).second)
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " name '" + desired_name +
                                        "' appears multiple times.");
        }

        resolved_indices.push_back(canonical_index);
    }

    if (!strict_full_order)
    {
        for (uint32_t i = 0u; i < canonical_names.size(); ++i)
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
 * @brief Resolves alias-based mappings to current layout indices.
 *
 * @param mappings Alias-to-semantic mappings.
 * @param current_aliases Ordered current aliases.
 * @param strict_full_order Whether mapping must cover all pins.
 * @param pin_kind Pin kind string used in diagnostics.
 * @return Ordered pairs of resolved index and semantic name.
 */
inline std::vector<std::pair<uint32_t, std::string>>
resolve_alias_mapping_indices(const std::vector<pin_alias_semantic_mapping>& mappings,
                              const std::vector<std::string>& current_aliases, const bool strict_full_order,
                              const std::string_view& pin_kind)
{
    const auto alias_to_index = build_name_to_index(current_aliases);

    std::unordered_set<uint32_t>                  used_indices{};
    std::unordered_set<std::string>               used_semantics{};
    std::vector<std::pair<uint32_t, std::string>> resolved{};

    used_indices.reserve(current_aliases.size());
    used_semantics.reserve(current_aliases.size());
    resolved.reserve(current_aliases.size());

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

        if (!used_semantics.insert(mapping.semantic_name).second)
        {
            throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " semantic name '" +
                                        mapping.semantic_name + "' appears multiple times.");
        }

        resolved.emplace_back(index, mapping.semantic_name);
    }

    if (strict_full_order && resolved.size() != current_aliases.size())
    {
        throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " mapping size (" +
                                    std::to_string(resolved.size()) + ") does not match layout count (" +
                                    std::to_string(current_aliases.size()) + ").");
    }

    if (!strict_full_order)
    {
        for (uint32_t i = 0u; i < current_aliases.size(); ++i)
        {
            if (used_indices.find(i) == used_indices.cend())
            {
                resolved.emplace_back(i, "");
            }
        }
    }

    return resolved;
}

/**
 * @brief Resolves canonical index for a semantic name.
 *
 * @param semantic_name Semantic name.
 * @param canonical_name_to_index Optional canonical map.
 * @param pin_kind Pin kind string used in diagnostics.
 * @return Canonical index or @ref unknown_canonical_index.
 */
inline uint32_t
resolve_canonical_index(const std::string&                                              semantic_name,
                        const std::optional<std::unordered_map<std::string, uint32_t>>& canonical_name_to_index,
                        const std::string_view&                                         pin_kind)
{
    if (!canonical_name_to_index.has_value())
    {
        return unknown_canonical_index;
    }

    const auto it = canonical_name_to_index->find(semantic_name);
    if (it == canonical_name_to_index->cend())
    {
        throw std::invalid_argument(std::string{"Desired "} + std::string{pin_kind} + " semantic name '" +
                                    semantic_name + "' is not present in canonical AIG names.");
    }

    return it->second;
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
        nlohmann::json canonical_value = nullptr;
        if (mapping.canonical_index != unknown_canonical_index)
        {
            canonical_value = mapping.canonical_index;
        }

        report_json["input_mappings"].push_back({{"semantic_name", mapping.semantic_name},
                                                 {"canonical_index", canonical_value},
                                                 {"fgl_alias", mapping.fgl_alias}});
    }

    for (const auto& mapping : report.output_mappings)
    {
        nlohmann::json canonical_value = nullptr;
        if (mapping.canonical_index != unknown_canonical_index)
        {
            canonical_value = mapping.canonical_index;
        }

        report_json["output_mappings"].push_back({{"semantic_name", mapping.semantic_name},
                                                  {"canonical_index", canonical_value},
                                                  {"fgl_alias", mapping.fgl_alias}});
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

    const auto use_input_order     = !config.input_order.empty();
    const auto use_output_order    = !config.output_order.empty();
    const auto use_input_mappings  = !config.input_mappings.empty();
    const auto use_output_mappings = !config.output_mappings.empty();

    if (!use_input_order && !use_input_mappings)
    {
        throw std::invalid_argument("No PI specification provided. Provide either `input_order` or `input_mappings`.");
    }

    if (!use_output_order && !use_output_mappings)
    {
        throw std::invalid_argument(
            "No PO specification provided. Provide either `output_order` or `output_mappings`.");
    }

    if ((use_input_order || use_output_order) && !config.aig_file.has_value())
    {
        throw std::invalid_argument("AIG file is required when using semantic pin orders.");
    }

    std::optional<std::vector<std::string>>                  canonical_inputs{};
    std::optional<std::vector<std::string>>                  canonical_outputs{};
    std::optional<std::unordered_map<std::string, uint32_t>> canonical_input_name_to_index{};
    std::optional<std::unordered_map<std::string, uint32_t>> canonical_output_name_to_index{};

    if (config.aig_file.has_value())
    {
        const auto [inputs, outputs]   = detail::extract_semantic_names_from_aig(*config.aig_file);
        canonical_inputs               = std::move(inputs);
        canonical_outputs              = std::move(outputs);
        canonical_input_name_to_index  = detail::build_name_to_index(*canonical_inputs);
        canonical_output_name_to_index = detail::build_name_to_index(*canonical_outputs);
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

    std::vector<mockturtle::node<Lyt>> current_pos{};
    current_pos.reserve(layout.num_pos());

    std::vector<std::string> current_po_aliases{};
    current_po_aliases.reserve(layout.num_pos());

    uint32_t po_index = 0u;
    layout.foreach_po(
        [&layout, &current_pos, &current_po_aliases, &po_index](const auto& po)
        {
            current_pos.push_back(layout.get_node(po));
            current_po_aliases.push_back(layout.get_output_name(po_index++));
        });

    if (canonical_inputs.has_value() && current_pis.size() != canonical_inputs->size())
    {
        throw std::invalid_argument("Input layout PI count does not match canonical AIG PI count.");
    }

    if (canonical_outputs.has_value() && current_pos.size() != canonical_outputs->size())
    {
        throw std::invalid_argument("Input layout PO count does not match canonical AIG PO count.");
    }

    std::vector<mockturtle::node<Lyt>> target_pis{};
    std::vector<mockturtle::node<Lyt>> target_pos{};

    pin_unscrambling_result<Lyt> result{};
    result.report.topology = detail::topology_label<Lyt>();
    result.report.num_pis  = static_cast<uint32_t>(current_pis.size());
    result.report.num_pos  = static_cast<uint32_t>(current_pos.size());

    // Resolve PI target order and report mapping.
    if (use_input_mappings)
    {
        const auto resolved = detail::resolve_alias_mapping_indices(config.input_mappings, current_pi_aliases,
                                                                    config.strict_full_order, "PI");
        target_pis.reserve(resolved.size());
        result.report.input_mappings.reserve(resolved.size());

        for (const auto& [index, semantic_raw] : resolved)
        {
            const auto semantic_name =
                semantic_raw.empty() ?
                    (canonical_inputs.has_value() ? (*canonical_inputs)[index] : current_pi_aliases[index]) :
                    semantic_raw;
            const auto canonical_index =
                detail::resolve_canonical_index(semantic_name, canonical_input_name_to_index, "PI");

            target_pis.push_back(current_pis[index]);
            result.report.input_mappings.push_back({semantic_name, canonical_index, current_pi_aliases[index]});
        }
    }
    else
    {
        const auto pi_target_indices =
            detail::resolve_order_indices(config.input_order, *canonical_inputs, config.strict_full_order, "PI");
        target_pis.reserve(pi_target_indices.size());
        result.report.input_mappings.reserve(pi_target_indices.size());

        for (const auto index : pi_target_indices)
        {
            target_pis.push_back(current_pis[index]);
            result.report.input_mappings.push_back({(*canonical_inputs)[index], index, current_pi_aliases[index]});
        }
    }

    // Resolve PO target order and report mapping.
    if (use_output_mappings)
    {
        const auto resolved = detail::resolve_alias_mapping_indices(config.output_mappings, current_po_aliases,
                                                                    config.strict_full_order, "PO");
        target_pos.reserve(resolved.size());
        result.report.output_mappings.reserve(resolved.size());

        for (const auto& [index, semantic_raw] : resolved)
        {
            const auto semantic_name =
                semantic_raw.empty() ?
                    (canonical_outputs.has_value() ? (*canonical_outputs)[index] : current_po_aliases[index]) :
                    semantic_raw;
            const auto canonical_index =
                detail::resolve_canonical_index(semantic_name, canonical_output_name_to_index, "PO");

            target_pos.push_back(current_pos[index]);
            result.report.output_mappings.push_back({semantic_name, canonical_index, current_po_aliases[index]});
        }
    }
    else
    {
        const auto po_target_indices =
            detail::resolve_order_indices(config.output_order, *canonical_outputs, config.strict_full_order, "PO");
        target_pos.reserve(po_target_indices.size());
        result.report.output_mappings.reserve(po_target_indices.size());

        for (const auto index : po_target_indices)
        {
            target_pos.push_back(current_pos[index]);
            result.report.output_mappings.push_back({(*canonical_outputs)[index], index, current_po_aliases[index]});
        }
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
