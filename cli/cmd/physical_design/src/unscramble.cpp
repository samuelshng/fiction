/**
 * @file unscramble.cpp
 * @brief CLI command implementation for store-based pin unscrambling.
 */

#include "cmd/physical_design/include/unscramble.hpp"

#include "stores.hpp"  // NOLINT(misc-include-cleaner)

#include <alice/alice.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{

/**
 * @brief Trims leading and trailing whitespace from a string.
 *
 * @param value String to trim.
 * @return Trimmed string.
 */
[[nodiscard]] std::string trim_whitespace(std::string value)
{
    const auto first =
        std::find_if_not(value.cbegin(), value.cend(), [](const unsigned char c) { return std::isspace(c) != 0; });
    if (first == value.cend())
    {
        return "";
    }

    const auto last =
        std::find_if_not(value.crbegin(), value.crend(), [](const unsigned char c) { return std::isspace(c) != 0; })
            .base();

    return {first, last};
}

/**
 * @brief Parses a comma-separated pin order list.
 *
 * @param csv Comma-separated list.
 * @param option_name Option name used in diagnostics.
 * @return Parsed pin names in provided order.
 */
[[nodiscard]] std::vector<std::string> parse_pin_order_csv(const std::string& csv, const std::string_view& option_name)
{
    if (csv.empty())
    {
        throw std::invalid_argument("`" + std::string{option_name} + "` requires a non-empty comma-separated list.");
    }

    std::vector<std::string> order{};
    std::string              current{};

    const auto flush_token = [&order, &current, &option_name]()
    {
        const auto token = trim_whitespace(current);
        if (token.empty())
        {
            throw std::invalid_argument("`" + std::string{option_name} +
                                        "` contains an empty token; expected comma-separated pin names.");
        }
        order.push_back(token);
        current.clear();
    };

    for (const auto c : csv)
    {
        if (c == ',')
        {
            flush_token();
        }
        else
        {
            current.push_back(c);
        }
    }

    flush_token();

    return order;
}

/**
 * @brief Parses one alias-to-semantic mapping item of the form `alias=semantic`.
 *
 * @param item Mapping item string.
 * @param option_name Option name used in diagnostics.
 * @return Parsed mapping entry.
 */
[[nodiscard]] fiction::pin_alias_semantic_mapping parse_mapping_item(const std::string&      item,
                                                                     const std::string_view& option_name)
{
    const auto delimiter_pos = item.find('=');

    if (delimiter_pos == std::string::npos)
    {
        throw std::invalid_argument("`" + std::string{option_name} + "` entry '" + item +
                                    "' is invalid. Expected format: <fgl_alias>=<semantic_name>.");
    }

    const auto alias    = trim_whitespace(item.substr(0, delimiter_pos));
    const auto semantic = trim_whitespace(item.substr(delimiter_pos + 1));

    if (alias.empty() || semantic.empty())
    {
        throw std::invalid_argument("`" + std::string{option_name} + "` entry '" + item +
                                    "' is invalid. Alias and semantic name must both be non-empty.");
    }

    return {alias, semantic};
}

/**
 * @brief Parses repeated alias-to-semantic mapping arguments.
 *
 * @param items Argument items.
 * @param option_name Option name used in diagnostics.
 * @return Parsed mapping entries.
 */
[[nodiscard]] std::vector<fiction::pin_alias_semantic_mapping> parse_mapping_args(const std::vector<std::string>& items,
                                                                                  const std::string_view& option_name)
{
    std::vector<fiction::pin_alias_semantic_mapping> mappings{};
    mappings.reserve(items.size());

    for (const auto& item : items)
    {
        mappings.push_back(parse_mapping_item(item, option_name));
    }

    return mappings;
}

}  // namespace

namespace alice
{

unscramble_command::unscramble_command(const environment::ptr& e) :
        command(e, "Unscrambles primary input and output pins of the current gate layout in store.")
{
    add_option("--spec-file", spec_file,
               "Optional pin unscrambling JSON specification file. CLI flags take precedence over this file.");
    add_option("--aig-file", aig_file,
               "AIG file used as canonical semantic I/O naming source. Overrides `aig_file` from spec file.");
    add_option("--input_order,--input-order", input_order,
               "Comma-separated semantic PI order. Ignored when `--input-mapping` is provided. Overrides `input_order`"
               " from spec file.");
    add_option("--input-mapping", input_mapping_args,
               "Alias-to-semantic PI mapping override entry in format '<fgl_alias>=<semantic_name>'. Repeat this "
               "option to provide multiple mappings. If any are provided, `input_mappings` from spec is ignored.");
    add_option("--output-order,--output_order", output_order,
               "Comma-separated semantic PO order. Ignored when `--output-mapping` is provided. Overrides "
               "`output_order` from spec file.");
    add_option("--output-mapping", output_mapping_args,
               "Alias-to-semantic PO mapping override entry in format '<fgl_alias>=<semantic_name>'. Repeat this "
               "option to provide multiple mappings. If any are provided, `output_mappings` from spec is ignored.");
    add_flag("--no-strict-full-order", no_strict_full_order,
             "Disable strict full-order checking and append missing pins in current layout order");
    add_option("--report-file", report_file,
               "Optional JSON report output file. Overrides `report_file` from spec file.");
    add_flag("--verbose,-v", "Print resolved semantic-to-layout pin mappings");
}

void unscramble_command::execute()
{
    auto& gls = store<fiction::gate_layout_t>();

    if (gls.empty())
    {
        env->out() << "[w] no gate layout in store\n";
        spec   = {};
        cfg    = {};
        report = {};
        return;
    }

    try
    {
        spec = is_set("spec-file") ? fiction::read_pin_unscrambling_spec(spec_file) : fiction::pin_unscrambling_spec{};

        cfg = {};

        if (is_set("aig-file"))
        {
            cfg.aig_file = aig_file;
        }
        else
        {
            cfg.aig_file = spec.aig_file;
        }

        const auto cli_input_mappings_override  = !input_mapping_args.empty();
        const auto cli_output_mappings_override = !output_mapping_args.empty();

        if (cli_input_mappings_override)
        {
            // CLI mapping overrides JSON mapping entirely; validation failure must not fall back.
            cfg.input_mappings = parse_mapping_args(input_mapping_args, "--input-mapping");
        }
        else
        {
            cfg.input_mappings = spec.input_mappings;
        }

        if (cli_output_mappings_override)
        {
            // CLI mapping overrides JSON mapping entirely; validation failure must not fall back.
            cfg.output_mappings = parse_mapping_args(output_mapping_args, "--output-mapping");
        }
        else
        {
            cfg.output_mappings = spec.output_mappings;
        }

        if (cfg.input_mappings.empty())
        {
            if (is_set("input_order") || is_set("input-order"))
            {
                cfg.input_order = parse_pin_order_csv(input_order, "--input_order");
            }
            else
            {
                cfg.input_order = spec.input_order;
            }
        }

        if (cfg.output_mappings.empty())
        {
            if (is_set("output-order") || is_set("output_order"))
            {
                cfg.output_order = parse_pin_order_csv(output_order, "--output-order");
            }
            else
            {
                cfg.output_order = spec.output_order;
            }
        }

        if (cfg.input_mappings.empty() && cfg.input_order.empty())
        {
            throw std::invalid_argument(
                "Missing required PI specification: provide input mappings or input order via CLI and/or spec.");
        }

        if (cfg.output_mappings.empty() && cfg.output_order.empty())
        {
            throw std::invalid_argument(
                "Missing required PO specification: provide output mappings or output order via CLI and/or spec.");
        }

        cfg.strict_full_order = true;
        if (!no_strict_full_order)
        {
            if (spec.strict_full_order.has_value())
            {
                cfg.strict_full_order = *spec.strict_full_order;
            }
        }
        else
        {
            cfg.strict_full_order = false;
        }

        if (is_set("report-file"))
        {
            cfg.report_file = report_file;
        }
        else
        {
            cfg.report_file = spec.report_file;
        }

        const auto apply_unscrambling = [this](auto&& lyt_ptr) -> std::optional<fiction::gate_layout_t>
        {
            using Lyt = typename std::decay_t<decltype(lyt_ptr)>::element_type;

            if constexpr (std::is_same_v<Lyt, fiction::hex_even_row_gate_clk_lyt> ||
                          std::is_same_v<Lyt, fiction::hex_odd_row_gate_clk_lyt>)
            {
                const auto result = fiction::run_pin_unscrambling(*lyt_ptr, cfg);
                report            = result.report;
                return std::make_shared<Lyt>(result.layout);
            }
            else
            {
                env->out() << "[e] pin unscrambling currently supports only even_row_hex and odd_row_hex layouts\n";
                return std::nullopt;
            }
        };

        if (const auto unscrambled = std::visit(apply_unscrambling, gls.current()); unscrambled.has_value())
        {
            gls.extend() = *unscrambled;

            env->out() << "[i] pin unscrambling completed\n";
            env->out() << fmt::format("[i] topology: {}\n", report.topology);
            env->out() << fmt::format("[i] PI count: {}, PO count: {}\n", report.num_pis, report.num_pos);
            env->out() << fmt::format("[i] runtime: {} seconds\n", report.total_runtime_seconds);

            if (cfg.report_file.has_value())
            {
                env->out() << fmt::format("[i] report file: {}\n", *cfg.report_file);
            }

            if (is_set("verbose"))
            {
                for (const auto& mapping : report.input_mappings)
                {
                    env->out() << fmt::format("[i] PI {} -> canonical {} (alias {})\n", mapping.semantic_name,
                                              mapping.canonical_index, mapping.fgl_alias);
                }

                for (const auto& mapping : report.output_mappings)
                {
                    env->out() << fmt::format("[i] PO {} -> canonical {} (alias {})\n", mapping.semantic_name,
                                              mapping.canonical_index, mapping.fgl_alias);
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        spec   = {};
        cfg    = {};
        report = {};
        env->out() << fmt::format("[e] {}\n", e.what());
    }
}

nlohmann::json unscramble_command::log() const
{
    return nlohmann::json{{"spec file", spec_file},
                          {"aig file", cfg.aig_file.value_or("")},
                          {"input mapping count", cfg.input_mappings.size()},
                          {"output mapping count", cfg.output_mappings.size()},
                          {"strict full order", cfg.strict_full_order},
                          {"topology", report.topology},
                          {"inputs", report.num_pis},
                          {"outputs", report.num_pos},
                          {"runtime in seconds", report.total_runtime_seconds}};
}

}  // namespace alice
