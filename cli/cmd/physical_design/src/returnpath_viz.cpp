//
// Created by marcel on 23.02.26.
//

#include "cmd/physical_design/include/returnpath_viz.hpp"

#include "stores.hpp"  // NOLINT(misc-include-cleaner)

#include <fiction/io/write_dot_return_path_overlay_layout.hpp>
#include <fiction/traits.hpp>
#include <fiction/types.hpp>
#include <fiction/utils/name_utils.hpp>

#include <alice/alice.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace
{

/**
 * @brief Visualization mode for return-path routing.
 */
enum class returnpath_viz_mode
{
    RELAXED,
    STRICT,
    BOTH
};

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
 * @brief Parses a non-negative 64-bit integer token.
 *
 * @param token Integer token.
 * @param option_name Option name for diagnostics.
 * @return Parsed value.
 * @throws std::invalid_argument If parsing fails.
 */
[[nodiscard]] uint64_t parse_uint64_token(const std::string& token, const std::string_view option_name)
{
    std::size_t consumed{};

    const auto parsed = std::stoull(token, &consumed);
    if (consumed != token.size())
    {
        throw std::invalid_argument(fmt::format("`{}` contains an invalid token: '{}'", option_name, token));
    }

    return parsed;
}

/**
 * @brief Parses the visualization mode string.
 *
 * @param mode Mode string.
 * @return Parsed visualization mode.
 * @throws std::invalid_argument If mode is unsupported.
 */
[[nodiscard]] returnpath_viz_mode parse_mode(std::string mode)
{
    std::transform(mode.begin(), mode.end(), mode.begin(), [](const unsigned char c) { return std::tolower(c); });

    if (mode == "relaxed")
    {
        return returnpath_viz_mode::RELAXED;
    }
    if (mode == "strict")
    {
        return returnpath_viz_mode::STRICT;
    }
    if (mode == "both")
    {
        return returnpath_viz_mode::BOTH;
    }

    throw std::invalid_argument("`--mode` must be one of: relaxed, strict, both.");
}

/**
 * @brief Parses comma-separated pair routing order indices.
 *
 * @param csv Comma-separated list of indices.
 * @return Parsed index list.
 * @throws std::invalid_argument If list is malformed.
 */
[[nodiscard]] std::vector<uint32_t> parse_pin_routing_order_csv(const std::string& csv)
{
    if (csv.empty())
    {
        return {};
    }

    std::vector<uint32_t> order{};
    std::string           current{};

    const auto flush_token = [&order, &current]()
    {
        const auto token = trim_whitespace(current);
        if (token.empty())
        {
            throw std::invalid_argument("`--pin_routing_order` contains an empty token.");
        }

        const auto parsed = parse_uint64_token(token, "--pin_routing_order");
        if (parsed > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument(fmt::format("`--pin_routing_order` token '{}' exceeds uint32 range.", token));
        }

        order.push_back(static_cast<uint32_t>(parsed));
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
 * @brief Parses comma-separated whitelist coordinates in `x:y` or `x:y:z` format.
 *
 * @param csv Comma-separated coordinate list.
 * @return Parsed whitelist coordinates.
 * @throws std::invalid_argument If list is malformed.
 */
[[nodiscard]] std::vector<fiction::route_return_path_coordinate> parse_routing_whitelist_csv(const std::string& csv)
{
    if (csv.empty())
    {
        return {};
    }

    std::vector<fiction::route_return_path_coordinate> whitelist{};
    std::string                                        current{};

    const auto flush_token = [&whitelist, &current]()
    {
        const auto token = trim_whitespace(current);
        if (token.empty())
        {
            throw std::invalid_argument("`--routing_whitelist` contains an empty token.");
        }

        std::vector<std::string> parts{};
        std::string              part{};
        for (const auto c : token)
        {
            if (c == ':')
            {
                parts.push_back(trim_whitespace(part));
                part.clear();
            }
            else
            {
                part.push_back(c);
            }
        }
        parts.push_back(trim_whitespace(part));

        if (parts.size() < 2u || parts.size() > 3u)
        {
            throw std::invalid_argument(
                fmt::format("`--routing_whitelist` token '{}' must have format x:y or x:y:z.", token));
        }

        const auto x = parse_uint64_token(parts[0], "--routing_whitelist");
        const auto y = parse_uint64_token(parts[1], "--routing_whitelist");
        const auto z = parts.size() == 3u ? parse_uint64_token(parts[2], "--routing_whitelist") : 0u;

        whitelist.push_back({x, y, z});
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

    return whitelist;
}

/**
 * @brief Converts mode enum to readable string.
 *
 * @param mode Mode.
 * @return Mode string.
 */
[[nodiscard]] std::string mode_to_string(const returnpath_viz_mode mode)
{
    switch (mode)
    {
        case returnpath_viz_mode::RELAXED: return "relaxed";
        case returnpath_viz_mode::STRICT: return "strict";
        case returnpath_viz_mode::BOTH: return "both";
        default: return "unknown";
    }
}

/**
 * @brief Creates a normalized DOT path.
 *
 * @param filename User-provided filename.
 * @param fallback_name Fallback filename.
 * @return Normalized DOT path.
 */
[[nodiscard]] std::filesystem::path normalized_dot_path(const std::string& filename, const std::string& fallback_name)
{
    auto path = filename.empty() ? std::filesystem::path{fallback_name} : std::filesystem::path{filename};

    if (path.extension() != ".dot")
    {
        path += ".dot";
    }

    return path;
}

/**
 * @brief Appends a mode suffix to a DOT path.
 *
 * @param base_dot_path Base DOT path.
 * @param suffix Suffix without extension.
 * @return Suffixed DOT path.
 */
[[nodiscard]] std::filesystem::path suffixed_dot_path(const std::filesystem::path& base_dot_path,
                                                      const std::string_view       suffix)
{
    const auto stem      = base_dot_path.stem().string();
    const auto extension = base_dot_path.extension().string();

    return base_dot_path.parent_path() / fmt::format("{}-{}{}", stem, suffix, extension);
}

/**
 * @brief Writes success status metadata.
 *
 * @tparam Lyt Gate-level layout type.
 * @param path Status output path.
 * @param mode Mode.
 * @param routed_layout Routed layout.
 * @param stats Routing statistics.
 * @param params Routing parameters.
 */
template <typename Lyt>
void write_success_status_file(const std::filesystem::path& path, const returnpath_viz_mode mode,
                               const Lyt& routed_layout, const fiction::route_return_path_stats& stats,
                               const fiction::route_return_path_params& params)
{
    std::ofstream out(path);

    out << "routing_status: success\n";
    out << "mode: " << mode_to_string(mode) << "\n";
    out << "allow_invalid_final_segment_clocking: " << (params.allow_invalid_final_segment_clocking ? "true" : "false")
        << "\n";
    out << "routed_pairs: " << stats.num_routed_pairs << "\n";
    out << "routed_segments: " << stats.num_routed_segments << "\n";

    bool all_pis_have_incoming_any_clock   = true;
    bool all_pis_have_incoming_clock_valid = true;

    routed_layout.foreach_pi(
        [&routed_layout, &all_pis_have_incoming_any_clock, &all_pis_have_incoming_clock_valid](const auto& pi)
        {
            all_pis_have_incoming_any_clock &= routed_layout.template fanin_size<false>(pi) > 0u;
            all_pis_have_incoming_clock_valid &= routed_layout.fanin_size(pi) > 0u;
        });

    out << "all_pis_have_incoming_any_clock: " << (all_pis_have_incoming_any_clock ? "true" : "false") << "\n";
    out << "all_pis_have_incoming_clock_valid: " << (all_pis_have_incoming_clock_valid ? "true" : "false") << "\n";
}

/**
 * @brief Writes failure status metadata.
 *
 * @param path Status output path.
 * @param mode Mode.
 * @param reason Failure reason.
 */
void write_failure_status_file(const std::filesystem::path& path, const returnpath_viz_mode mode,
                               const std::string_view reason)
{
    std::ofstream out(path);

    out << "routing_status: failed\n";
    out << "mode: " << mode_to_string(mode) << "\n";
    out << "reason: " << reason << "\n";
}

}  // namespace

namespace alice
{

returnpath_viz_command::returnpath_viz_command(const environment::ptr& e) :
        command(e, "Visualizes return-path routing on the current gate-level layout and highlights added return-path "
                   "segments in DOT output.")
{
    add_option("filename", filename,
               "Output DOT filename (single mode) or base filename for generated mode-suffixed files (both mode)");
    add_option("--mode", mode, "Visualization mode: relaxed, strict, or both", true)
        ->set_type_name("{relaxed,strict,both}");

    add_option("--top_margin", ps.top_margin, "Requested top routing margin", true);
    add_option("--bottom_margin", ps.bottom_margin, "Requested bottom routing margin", true);
    add_option("--right_margin", ps.right_margin, "Requested right routing margin", true);
    add_option("--lane_spacing", ps.lane_spacing, "Spacing between neighboring return-path lanes", true);

    add_option("--pin_routing_order", pin_routing_order,
               "Optional comma-separated pair routing order (indices of PO[i] -> PI[i] pairs)");
    add_option("--routing_whitelist", routing_whitelist,
               "Optional comma-separated whitelist coordinates in x:y or x:y:z format");
}

void returnpath_viz_command::execute()
{
    auto& s = store<fiction::gate_layout_t>();

    if (s.empty())
    {
        env->out() << "[w] no gate-level layout in store\n";
        return;
    }

    if (ps.lane_spacing == 0u)
    {
        env->out() << "[e] --lane_spacing must be greater than 0\n";
        return;
    }

    returnpath_viz_mode viz_mode{};

    try
    {
        viz_mode             = parse_mode(mode);
        ps.pin_routing_order = parse_pin_routing_order_csv(pin_routing_order);
        ps.routing_whitelist = parse_routing_whitelist_csv(routing_whitelist);
    }
    catch (const std::invalid_argument& e)
    {
        env->out() << fmt::format("[e] {}\n", e.what());
        return;
    }

    const auto& current_layout = s.current();

    const auto visualize_layout = [this, viz_mode](auto&& lyt_ptr)
    {
        using Lyt = typename std::decay_t<decltype(lyt_ptr)>::element_type;

        if constexpr (fiction::is_gate_level_layout_v<Lyt>)
        {
            constexpr bool is_supported_pointy_hex_layout = std::is_same_v<Lyt, fiction::hex_odd_row_gate_clk_lyt> ||
                                                            std::is_same_v<Lyt, fiction::hex_even_row_gate_clk_lyt>;

            if constexpr (is_supported_pointy_hex_layout)
            {
                if (!lyt_ptr->is_clocking_scheme(fiction::clock_name::ROW))
                {
                    env->out() << "[e] returnpath_viz requires a row-clocked layout\n";
                    return;
                }

                const auto default_filename = fmt::format("{}_returnpath_viz.dot", fiction::get_name(*lyt_ptr));
                const auto base_dot_path    = normalized_dot_path(filename, default_filename);

                const auto run_mode = [this, viz_mode, &lyt_ptr](const returnpath_viz_mode         current_mode,
                                                                 const std::filesystem::path&      dot_path,
                                                                 fiction::route_return_path_params mode_params)
                {
                    const bool strict_mode                           = current_mode == returnpath_viz_mode::STRICT;
                    mode_params.allow_invalid_final_segment_clocking = !strict_mode;

                    if (const auto parent = dot_path.parent_path(); !parent.empty())
                    {
                        std::filesystem::create_directories(parent);
                    }

                    const auto status_path = dot_path.parent_path() / fmt::format("{}.txt", dot_path.stem().string());

                    try
                    {
                        fiction::route_return_path_stats stats{};
                        const auto routed_layout = fiction::route_return_path(*lyt_ptr, mode_params, &stats);
                        const auto reference_layout =
                            fiction::create_return_path_reference_layout(*lyt_ptr, mode_params);

                        fiction::return_path_overlay_drawer_params drawer_params{};
                        drawer_params.respect_clocking = strict_mode;

                        fiction::write_dot_return_path_overlay_layout(reference_layout, routed_layout,
                                                                      dot_path.string(), drawer_params);
                        write_success_status_file(status_path, current_mode, routed_layout, stats, mode_params);

                        env->out() << fmt::format("[i] wrote {}\n", dot_path.string());
                        env->out() << fmt::format("[i] wrote {}\n", status_path.string());
                        return true;
                    }
                    catch (const std::exception& e)
                    {
                        write_failure_status_file(status_path, current_mode, e.what());
                        env->out() << fmt::format("[i] wrote {}\n", status_path.string());

                        if (viz_mode == returnpath_viz_mode::BOTH)
                        {
                            env->out() << fmt::format("[w] {} mode failed: {}\n", mode_to_string(current_mode),
                                                      e.what());
                        }
                        else
                        {
                            env->out() << fmt::format("[e] {}\n", e.what());
                        }

                        return false;
                    }
                };

                if (viz_mode == returnpath_viz_mode::BOTH)
                {
                    const auto relaxed_path = suffixed_dot_path(base_dot_path, "clock-relaxed");
                    const auto strict_path  = suffixed_dot_path(base_dot_path, "clock-strict");

                    auto relaxed_params = ps;
                    auto strict_params  = ps;

                    const auto relaxed_ok = run_mode(returnpath_viz_mode::RELAXED, relaxed_path, relaxed_params);
                    const auto strict_ok  = run_mode(returnpath_viz_mode::STRICT, strict_path, strict_params);

                    if (!relaxed_ok && !strict_ok)
                    {
                        env->out() << "[e] both relaxed and strict return-path visualizations failed\n";
                    }

                    return;
                }

                auto mode_params = ps;
                run_mode(viz_mode, base_dot_path, mode_params);
            }
            else
            {
                env->out() << "[e] returnpath_viz supports row-arranged pointy-top hexagonal gate-level layouts only\n";
            }
        }
        else
        {
            env->out() << "[e] returnpath_viz supports gate-level layouts only\n";
        }
    };

    std::visit(visualize_layout, current_layout);
}

}  // namespace alice
