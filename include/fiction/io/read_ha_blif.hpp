//
// Created by codex on 01.03.26.
//

#ifndef FICTION_READ_HA_BLIF_HPP
#define FICTION_READ_HA_BLIF_HPP

#include <kitty/constructors.hpp>
#include <kitty/dynamic_truth_table.hpp>
#include <lorina/blif.hpp>
#include <lorina/common.hpp>
#include <lorina/diagnostics.hpp>
#include <mockturtle/traits.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace fiction
{

namespace detail
{

/**
 * Removes leading and trailing whitespace from a string view.
 *
 * @param text String view to trim.
 * @return Trimmed copy of `text`.
 */
[[nodiscard]] inline std::string trim_copy(const std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
    {
        return {};
    }

    const auto end = text.find_last_not_of(" \t\r\n");

    return std::string{text.substr(begin, end - begin + 1)};
}

/**
 * Strips BLIF comments that start with '#'.
 *
 * @param line Line to strip.
 * @return Line contents before comment marker.
 */
[[nodiscard]] inline std::string strip_comment(const std::string_view line)
{
    if (const auto hash_pos = line.find('#'); hash_pos != std::string_view::npos)
    {
        return std::string{line.substr(0, hash_pos)};
    }

    return std::string{line};
}

/**
 * Converts a string to lower-case.
 *
 * @param text String to convert.
 * @return Lower-case copy.
 */
[[nodiscard]] inline std::string to_lower_copy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return text;
}

/**
 * Splits a string on whitespace.
 *
 * @param text Input string.
 * @return Tokens of `text`.
 */
[[nodiscard]] inline std::vector<std::string> split_whitespace(const std::string_view text)
{
    std::istringstream       iss{std::string{text}};
    std::vector<std::string> tokens{};
    std::string              token{};

    while (iss >> token)
    {
        tokens.push_back(token);
    }

    return tokens;
}

/**
 * Returns whether a BLIF line is a declaration/directive.
 *
 * @param line BLIF line.
 * @return `true` iff the line starts with '.'.
 */
[[nodiscard]] inline bool is_blif_directive(const std::string_view line)
{
    return !line.empty() && line.front() == '.';
}

/**
 * Emits a parse-line diagnostic if a diagnostic engine is available.
 *
 * @param diag Diagnostic engine.
 * @param line Line that could not be parsed.
 */
inline void report_parse_error(lorina::diagnostic_engine* diag, const std::string& line)
{
    if (diag != nullptr)
    {
        diag->report(lorina::diag_id::ERR_PARSE_LINE).add_argument(line);
    }
}

/**
 * Emits an unresolved dependency warning if a diagnostic engine is available.
 *
 * @param diag Diagnostic engine.
 * @param missing_name Missing net name.
 */
inline void report_unresolved_dependency(lorina::diagnostic_engine* diag, const std::string& missing_name)
{
    if (diag != nullptr)
    {
        diag->report(lorina::diag_id::WRN_UNRESOLVED_DEPENDENCY).add_argument(missing_name).add_argument("gate");
    }
}

/**
 * Operation representing one `.names` declaration.
 */
struct names_operation
{
    /**
     * Input net names.
     */
    std::vector<std::string> inputs{};
    /**
     * Output net name.
     */
    std::string output{};
    /**
     * N-input/1-output cover.
     */
    lorina::blif_reader::output_cover_t cover{};
};

/**
 * Operation representing one HA cell instantiation.
 */
struct ha_operation
{
    /**
     * First input net name.
     */
    std::string a{};
    /**
     * Second input net name.
     */
    std::string b{};
    /**
     * Carry output net name.
     */
    std::string carry{};
    /**
     * Sum output net name.
     */
    std::string sum{};
};

/**
 * Helper that builds a logic network from parsed HA-capable BLIF statements.
 *
 * @tparam Ntk Logic network type.
 */
template <typename Ntk>
class ha_blif_builder
{
  private:
    using signal    = mockturtle::signal<Ntk>;
    using operation = std::variant<names_operation, ha_operation>;

  public:
    /**
     * Standard constructor.
     *
     * @param ntk Network to be built.
     */
    explicit ha_blif_builder(Ntk& ntk) : network{ntk}
    {
        // Pre-compute literals for a half-adder multi-output node in block-network order:
        // output pin 0 = carry (AND), output pin 1 = sum (XOR).
        kitty::create_from_binary_string(and_tt, "1000");
        kitty::create_from_binary_string(xor_tt, "0110");
    }

    /**
     * Stores model name.
     *
     * @param model Model name.
     */
    void set_model_name(const std::string& model)
    {
        model_name = model;
    }

    /**
     * Declares primary inputs.
     *
     * @param input_names Input names.
     */
    void add_inputs(const std::vector<std::string>& input_names)
    {
        for (const auto& input_name : input_names)
        {
            const auto pi       = network.create_pi();
            signals[input_name] = pi;

            if constexpr (mockturtle::has_set_name_v<Ntk>)
            {
                network.set_name(pi, input_name);
            }
        }
    }

    /**
     * Declares primary outputs.
     *
     * @param output_names Output names.
     */
    void add_outputs(const std::vector<std::string>& output_names)
    {
        outputs.insert(outputs.end(), output_names.cbegin(), output_names.cend());
    }

    /**
     * Adds a deferred `.names` operation.
     *
     * @param op Operation to add.
     */
    void add_operation(const names_operation& op)
    {
        operations.emplace_back(op);
    }

    /**
     * Adds a deferred HA operation.
     *
     * @param op Operation to add.
     */
    void add_operation(const ha_operation& op)
    {
        operations.emplace_back(op);
    }

    /**
     * Builds all deferred gates and creates primary outputs.
     *
     * @param diag Optional diagnostics engine.
     * @return `true` iff all operations and POs could be resolved.
     */
    [[nodiscard]] bool build(lorina::diagnostic_engine* diag)
    {
        if (!model_name.empty())
        {
            if constexpr (mockturtle::has_set_network_name_v<Ntk>)
            {
                network.set_network_name(model_name);
            }
        }

        std::vector<bool> done(operations.size(), false);
        auto              resolved = true;

        while (true)
        {
            bool progress = false;

            for (auto index = 0u; index < operations.size(); ++index)
            {
                if (done[index])
                {
                    continue;
                }

                const auto operation_resolved =
                    std::visit([this](const auto& op) { return this->resolve_operation(op); }, operations[index]);

                if (operation_resolved)
                {
                    done[index] = true;
                    progress    = true;
                }
            }

            if (!progress)
            {
                break;
            }
        }

        if (std::any_of(done.cbegin(), done.cend(), [](const auto entry) { return !entry; }))
        {
            resolved = false;
            for (auto index = 0u; index < operations.size(); ++index)
            {
                if (!done[index])
                {
                    std::visit(
                        [diag](const auto& op)
                        {
                            if constexpr (std::is_same_v<std::decay_t<decltype(op)>, names_operation>)
                            {
                                for (const auto& input_name : op.inputs)
                                {
                                    report_unresolved_dependency(diag, input_name);
                                }
                            }
                            else if constexpr (std::is_same_v<std::decay_t<decltype(op)>, ha_operation>)
                            {
                                report_unresolved_dependency(diag, op.a);
                                report_unresolved_dependency(diag, op.b);
                            }
                        },
                        operations[index]);
                }
            }
        }

        for (auto output_index = 0u; output_index < outputs.size(); ++output_index)
        {
            if (const auto output_signal = resolve_signal(outputs[output_index]); output_signal.has_value())
            {
                network.create_po(*output_signal);

                if constexpr (mockturtle::has_set_output_name_v<Ntk>)
                {
                    network.set_output_name(output_index, outputs[output_index]);
                }
            }
            else
            {
                report_unresolved_dependency(diag, outputs[output_index]);
                resolved = false;
            }
        }

        return resolved;
    }

  private:
    /**
     * Resolves a `.names` operation.
     *
     * @param op Operation to resolve.
     * @return `true` iff all dependent inputs are known.
     */
    [[nodiscard]] bool resolve_operation(const names_operation& op)
    {
        std::vector<signal> input_signals{};
        input_signals.reserve(op.inputs.size());

        for (const auto& input_name : op.inputs)
        {
            if (const auto input_signal = resolve_signal(input_name); input_signal.has_value())
            {
                input_signals.push_back(*input_signal);
            }
            else
            {
                return false;
            }
        }

        signal output_signal = network.get_constant(false);

        if (input_signals.empty())
        {
            if (op.cover.empty())
            {
                output_signal = network.get_constant(false);
            }
            else if (op.cover.size() == 1u && op.cover.front().first.empty() && op.cover.front().second.size() == 1u)
            {
                output_signal = network.get_constant(op.cover.front().second.front() == '1');
            }
            else
            {
                return false;
            }
        }
        else
        {
            std::vector<kitty::cube> minterms{};
            std::vector<kitty::cube> maxterms{};

            for (const auto& row : op.cover)
            {
                if (row.second.size() != 1u)
                {
                    return false;
                }

                if (row.second.front() == '1')
                {
                    minterms.emplace_back(kitty::cube(row.first));
                }
                else if (row.second.front() == '0')
                {
                    maxterms.emplace_back(~kitty::cube(row.first));
                }
                else
                {
                    return false;
                }
            }

            if (!minterms.empty() && !maxterms.empty())
            {
                return false;
            }

            kitty::dynamic_truth_table tt{static_cast<uint32_t>(input_signals.size())};
            if (!minterms.empty())
            {
                kitty::create_from_cubes(tt, minterms, false);
            }
            else if (!maxterms.empty())
            {
                kitty::create_from_clauses(tt, maxterms, false);
            }

            output_signal = network.create_node(input_signals, tt);
        }

        signals[op.output] = output_signal;
        if constexpr (mockturtle::has_set_name_v<Ntk>)
        {
            network.set_name(output_signal, op.output);
        }

        return true;
    }

    /**
     * Resolves an HA operation.
     *
     * @param op Operation to resolve.
     * @return `true` iff all dependent inputs are known.
     */
    [[nodiscard]] bool resolve_operation(const ha_operation& op)
    {
        const auto a_signal = resolve_signal(op.a);
        const auto b_signal = resolve_signal(op.b);

        if (!a_signal.has_value() || !b_signal.has_value())
        {
            return false;
        }

        const auto ha_signal = static_cast<mockturtle::block_network&>(network).create_node(
            {*a_signal, *b_signal}, std::vector<kitty::dynamic_truth_table>{and_tt, xor_tt});

        const auto ha_node      = network.get_node(ha_signal);
        const auto carry_signal = network.make_signal(ha_node, 0u);
        const auto sum_signal   = network.make_signal(ha_node, 1u);

        signals[op.carry] = carry_signal;
        signals[op.sum]   = sum_signal;

        if constexpr (mockturtle::has_set_name_v<Ntk>)
        {
            network.set_name(carry_signal, op.carry);
            network.set_name(sum_signal, op.sum);
        }

        return true;
    }

    /**
     * Resolves a signal by BLIF name.
     *
     * Recognized constants are: `0`, `1`, `$false`, `$true`, `false`, and `true`.
     *
     * @param name BLIF net name.
     * @return Signal if known.
     */
    [[nodiscard]] std::optional<signal> resolve_signal(const std::string& name) const
    {
        if (const auto signal_it = signals.find(name); signal_it != signals.cend())
        {
            return signal_it->second;
        }

        const auto lowered_name = to_lower_copy(name);

        if (name == "0" || lowered_name == "$false" || lowered_name == "false")
        {
            return network.get_constant(false);
        }

        if (name == "1" || lowered_name == "$true" || lowered_name == "true")
        {
            return network.get_constant(true);
        }

        return std::nullopt;
    }

    /**
     * Network being constructed.
     */
    Ntk& network;
    /**
     * Optional model name.
     */
    std::string model_name{};
    /**
     * Known signals by BLIF net name.
     */
    std::unordered_map<std::string, signal> signals{};
    /**
     * Primary output names.
     */
    std::vector<std::string> outputs{};
    /**
     * Deferred operations to resolve in topological order.
     */
    std::vector<operation> operations{};
    /**
     * 2-input AND truth table.
     */
    kitty::dynamic_truth_table and_tt{2};
    /**
     * 2-input XOR truth table.
     */
    kitty::dynamic_truth_table xor_tt{2};
};

/**
 * Parses pin assignments from `.gate`/`.subckt` tokens.
 *
 * @param tokens Tokenized directive line.
 * @return Mapping of pin names to net names.
 */
[[nodiscard]] inline std::unordered_map<std::string, std::string>
extract_pin_assignments(const std::vector<std::string>& tokens)
{
    std::unordered_map<std::string, std::string> assignments{};

    for (auto index = 2u; index < tokens.size(); ++index)
    {
        const auto& token = tokens[index];
        if (const auto equals_pos = token.find('='); equals_pos != std::string::npos)
        {
            const auto pin_name = to_lower_copy(trim_copy(token.substr(0, equals_pos)));
            const auto net_name = trim_copy(token.substr(equals_pos + 1));

            if (!pin_name.empty() && !net_name.empty())
            {
                assignments[pin_name] = net_name;
            }
        }
    }

    return assignments;
}

/**
 * Extracts an HA operation from `.gate`/`.subckt` assignments.
 *
 * Supported aliases:
 * - Inputs: `A/B`, `a/b`
 * - Carry output: `X` or `C` (case-insensitive)
 * - Sum output: `Y` or `S` (case-insensitive)
 *
 * @param assignments Mapping of pin names to net names.
 * @return Parsed HA operation if all required pins are present.
 */
[[nodiscard]] inline std::optional<ha_operation>
parse_ha_operation(const std::unordered_map<std::string, std::string>& assignments)
{
    const auto a_it = assignments.find("a");
    const auto b_it = assignments.find("b");

    if (a_it == assignments.cend() || b_it == assignments.cend())
    {
        return std::nullopt;
    }

    auto carry_it = assignments.find("x");
    if (carry_it == assignments.cend())
    {
        carry_it = assignments.find("c");
    }

    auto sum_it = assignments.find("y");
    if (sum_it == assignments.cend())
    {
        sum_it = assignments.find("s");
    }

    if (carry_it == assignments.cend() || sum_it == assignments.cend())
    {
        return std::nullopt;
    }

    return ha_operation{a_it->second, b_it->second, carry_it->second, sum_it->second};
}

/**
 * Parses a BLIF file that can contain `.names` as well as explicit HA cells in `.gate`/`.subckt` form.
 *
 * @tparam Ntk Logic network type.
 * @param filename BLIF input file.
 * @param ntk Destination network.
 * @param diag Optional diagnostics engine.
 * @return Parsing status code.
 */
template <typename Ntk>
[[nodiscard]] lorina::return_code read_ha_blif(const std::string& filename, Ntk& ntk, lorina::diagnostic_engine* diag)
{
    std::ifstream in{filename};
    if (!in.is_open())
    {
        if (diag != nullptr)
        {
            diag->report(lorina::diag_id::ERR_FILE_OPEN).add_argument(filename);
        }
        return lorina::return_code::parse_error;
    }

    std::vector<std::string> lines{};
    std::string              raw_line{};
    std::string              continued_line{};

    while (std::getline(in, raw_line))
    {
        auto line = trim_copy(strip_comment(raw_line));
        if (line.empty())
        {
            continue;
        }

        if (!continued_line.empty())
        {
            continued_line += " ";
            continued_line += line;
        }
        else
        {
            continued_line = std::move(line);
        }

        if (!continued_line.empty() && continued_line.back() == '\\')
        {
            continued_line.pop_back();
            continued_line = trim_copy(continued_line);
            continue;
        }

        lines.push_back(continued_line);
        continued_line.clear();
    }

    if (!continued_line.empty())
    {
        lines.push_back(continued_line);
    }

    ha_blif_builder<Ntk> builder{ntk};

    for (auto line_idx = 0u; line_idx < lines.size(); ++line_idx)
    {
        const auto& line = lines[line_idx];

        if (!is_blif_directive(line))
        {
            report_parse_error(diag, line);
            return lorina::return_code::parse_error;
        }

        const auto tokens = split_whitespace(line);
        if (tokens.empty())
        {
            continue;
        }

        const auto directive = to_lower_copy(tokens[0]);

        if (directive == ".model")
        {
            if (tokens.size() < 2u)
            {
                report_parse_error(diag, line);
                return lorina::return_code::parse_error;
            }
            builder.set_model_name(tokens[1]);
        }
        else if (directive == ".inputs")
        {
            if (tokens.size() < 2u)
            {
                report_parse_error(diag, line);
                return lorina::return_code::parse_error;
            }

            builder.add_inputs(std::vector<std::string>{tokens.cbegin() + 1, tokens.cend()});
        }
        else if (directive == ".outputs")
        {
            if (tokens.size() < 2u)
            {
                report_parse_error(diag, line);
                return lorina::return_code::parse_error;
            }

            builder.add_outputs(std::vector<std::string>{tokens.cbegin() + 1, tokens.cend()});
        }
        else if (directive == ".names")
        {
            if (tokens.size() < 2u)
            {
                report_parse_error(diag, line);
                return lorina::return_code::parse_error;
            }

            names_operation names_op{};
            names_op.output = tokens.back();
            if (tokens.size() > 2u)
            {
                names_op.inputs = std::vector<std::string>{tokens.cbegin() + 1, tokens.cend() - 1};
            }

            while (line_idx + 1u < lines.size() && !is_blif_directive(lines[line_idx + 1u]))
            {
                const auto cover_line_tokens = split_whitespace(lines[++line_idx]);
                if (cover_line_tokens.size() == 1u)
                {
                    names_op.cover.emplace_back("", cover_line_tokens.front());
                }
                else if (cover_line_tokens.size() == 2u)
                {
                    names_op.cover.emplace_back(cover_line_tokens[0], cover_line_tokens[1]);
                }
                else
                {
                    report_parse_error(diag, lines[line_idx]);
                    return lorina::return_code::parse_error;
                }
            }

            builder.add_operation(names_op);
        }
        else if (directive == ".gate" || directive == ".subckt")
        {
            if (tokens.size() < 2u || to_lower_copy(tokens[1]) != "ha")
            {
                report_parse_error(diag, line);
                return lorina::return_code::parse_error;
            }

            if (const auto ha_op = parse_ha_operation(extract_pin_assignments(tokens)); ha_op.has_value())
            {
                builder.add_operation(*ha_op);
            }
            else
            {
                report_parse_error(diag, line);
                return lorina::return_code::parse_error;
            }
        }
        else if (directive == ".end")
        {
            break;
        }
        else
        {
            report_parse_error(diag, line);
            return lorina::return_code::parse_error;
        }
    }

    if (!builder.build(diag))
    {
        return lorina::return_code::parse_error;
    }

    return lorina::return_code::success;
}

}  // namespace detail

/**
 * Parses a BLIF file that contains regular `.names` logic and explicit HA cell declarations.
 *
 * @tparam Ntk Logic network type.
 * @param filename BLIF input file.
 * @param ntk Destination network.
 * @param diag Optional diagnostics engine.
 * @return Parsing status code.
 */
template <typename Ntk>
[[nodiscard]] lorina::return_code read_ha_blif(const std::string& filename, Ntk& ntk,
                                               lorina::diagnostic_engine* diag = nullptr)
{
    return detail::read_ha_blif(filename, ntk, diag);
}

}  // namespace fiction

#endif  // FICTION_READ_HA_BLIF_HPP
