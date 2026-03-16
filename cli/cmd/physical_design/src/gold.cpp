//
// Created by simon on 12.06.2024.
//

#include "cmd/physical_design/include/gold.hpp"

#include "stores.hpp"  // NOLINT(misc-include-cleaner)

#include <fiction/algorithms/physical_design/graph_oriented_layout_design.hpp>
#include <fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp>
#include <fiction/types.hpp>
#include <fiction/utils/name_utils.hpp>
#include <fiction/utils/network_utils.hpp>

#include <alice/alice.hpp>
#include <mockturtle/utils/stopwatch.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace
{

/**
 * Trims leading and trailing whitespace from a string.
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
 * Parses a comma-separated PI name list.
 *
 * @param csv Comma-separated PI names.
 * @return Parsed PI names in the given order.
 * @throws std::invalid_argument If the list is malformed.
 */
[[nodiscard]] std::vector<std::string> parse_input_pin_order_csv(const std::string& csv)
{
    if (csv.empty())
    {
        throw std::invalid_argument("`--input_pin_order` requires a non-empty comma-separated PI name list.");
    }

    std::vector<std::string> order{};
    std::string              current{};

    const auto flush_token = [&order, &current]()
    {
        const auto token = trim_whitespace(current);
        if (token.empty())
        {
            throw std::invalid_argument(
                "`--input_pin_order` contains an empty token; expected comma-separated PI names.");
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
 * Parses a comma-separated PO name list.
 *
 * @param csv Comma-separated PO names.
 * @return Parsed PO names in the given order.
 * @throws std::invalid_argument If the list is malformed.
 */
[[nodiscard]] std::vector<std::string> parse_output_pin_order_csv(const std::string& csv)
{
    if (csv.empty())
    {
        throw std::invalid_argument("`--output_pin_order` requires a non-empty comma-separated PO name list.");
    }

    std::vector<std::string> order{};
    std::string              current{};

    const auto flush_token = [&order, &current]()
    {
        const auto token = trim_whitespace(current);
        if (token.empty())
        {
            throw std::invalid_argument(
                "`--output_pin_order` contains an empty token; expected comma-separated PO names.");
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

}  // namespace

namespace alice
{

gold_command::gold_command(const environment::ptr& e) :
        command(e, "Performs scalable placement and routing of the current logic network in store using the "
                   "Graph-Oriented Layout Design (GOLD) algorithm. GOLD generates close-to-optimal 2DDWave-clocked "
                   "FCN gate-level layouts in reasonable runtime. Its result quality is better than 'ortho' and "
                   "its runtime behavior superior to 'exact' and 'onepass'. Additionally, different cost "
                   "objectives can be specified.")
{
    add_option("--timeout,-t", ps.timeout, "Timeout in seconds");
    add_option("--num_vertex_expansions,-n", ps.num_vertex_expansions, "Number of vertex expansions during search",
               true);
    add_option("--effort_mode,-e", ps.mode,
               "Specify the effort mode of the graph-oriented layout design algorithm. Possible values for the "
               "effort mode:\n"
               " - `0` (high_efficiency): Uses minimal computational resources, resulting in fewer search space graphs "
               "and potentially lower quality solutions.\n"
               " - `1` (high_effort): Uses more computational resources, creating more search space graphs to "
               "improve the likelihood of finding optimal solutions.\n"
               " - `2` (highest_effort): Uses even more computational resources, generating four to five times as many "
               "search space graphs compared to high-effort mode.\n"
               " - `3` (maximum_effort): Uses the maximum computational resources, generating the most search "
               "space graphs to ensure the highest chance of finding the best solution.",
               true)
        ->set_type_name("{high_efficiency=0, high_effort=1, highest_effort=2, maximum_effort=3}");
    add_option("--cost_objective,-c", ps.cost,
               "Specify the cost objective for the graph-oriented layout design algorithm. "
               "Possible values for the cost objective:\n"
               " - `0` (area): Minimize the layout area.\n- `1` (wires): Minimize the number of wire segments.\n"
               " - `2` (crossings): Minimize the number of crossings.\n"
               " - `3` (acp): Minimize the area-crossing product (ACP), balancing area and crossings.",
               true)
        ->set_type_name("{area=0, wires=1, crossings=2, acp=3}");
    add_flag("--return_first,-r", ps.return_first,
             "Terminate on the first found layout; reduces runtime but might sacrifice result quality");
    add_flag("--planar,-p", ps.planar, "Enable planar layout generation");
    add_flag("--multithreading,-m", ps.enable_multithreading, "Enable multithreading (beta feature)");
    add_flag("--verbose,-v", ps.verbose, "Be verbose");
    add_option("--seed,-s", seed,
               "Random seed used for random fanout substitution and random topological ordering in "
               "maximum-effort mode");
    add_flag("--straight_inverters,-i", ps.straight_inverters, "Enforce NOT gates to be routed non-bending only");
    add_flag("--prefer_input_pin_order", ps.prefer_input_pin_order,
             "Prefer primary inputs to be placed from left to right in network PI order");
    add_flag("--enforce_input_pin_order", ps.prefer_input_pin_order, "Deprecated alias for --prefer_input_pin_order");
    add_option("--input_pin_order", input_pin_order,
               "Comma-separated PI names defining preferred left-to-right PI order. Implies "
               "`--prefer_input_pin_order`.");
    add_flag("--prefer_output_pin_order", ps.prefer_output_pin_order,
             "Prefer primary outputs to be placed from left to right in network PO order");
    add_flag("--enforce_output_pin_order", ps.prefer_output_pin_order,
             "Deprecated alias for --prefer_output_pin_order");
    add_option("--output_pin_order", output_pin_order,
               "Comma-separated PO names defining preferred left-to-right PO order. Implies "
               "`--prefer_output_pin_order`.");
    add_option(
        "--tiles_to_skip_between_pis,-g", ps.tiles_to_skip_between_pis,
        "For each primary input (PI) considered during placement, reserve this many empty tiles after the current "
        "frontier to the right of the rightmost occupied tile or below the bottommost occupied tile before "
        "proposing a new PI position. This soft margin can reduce local congestion and increase the probability of "
        "finding a routable layout at the expense of a temporarily larger footprint, which post-layout "
        "optimization may later shrink. Defaults to 0.");
    add_flag("--randomize_tiles_to_skip_between_pis,-j", ps.randomize_tiles_to_skip_between_pis,
             "Randomize the number of skipped tiles for each PI placement. When enabled, each PI will use a "
             "random number of skipped tiles between tiles_to_skip_between_pis-1 and tiles_to_skip_between_pis "
             "(inclusive). When tiles_to_skip_between_pis is 0, only 0 will be used. This can help explore different "
             "placement strategies and potentially find better layouts. Requires a valid seed to be set for "
             "reproducible results.");
    add_option("--grid", grid, "Target grid for GOLD. Supported values: 'cartesian' (default) and 'hex'.", true)
        ->set_type_name("{cartesian,hex}");
}

void gold_command::execute()
{
    const auto reset_command_state = [this]() noexcept
    {
        ps   = {};
        grid = "cartesian";
        input_pin_order.clear();
        output_pin_order.clear();
    };

    // error case: empty logic network store
    if (store<fiction::logic_network_t>().empty())
    {
        env->out() << "[w] no logic network in store\n";
        reset_command_state();
        return;
    }

    if (ps.num_vertex_expansions == 0)
    {
        env->out() << "[w] the number of vertex expansions has to be at least 1\n";
        reset_command_state();
        return;
    }

    if (is_set("timeout"))
    {
        // convert timeout entered in seconds to milliseconds
        ps.timeout *= 1000;
    }

    if (is_set("seed"))
    {
        ps.seed = seed;
    }

    if (is_set("input_pin_order"))
    {
        try
        {
            ps.input_pin_order        = parse_input_pin_order_csv(input_pin_order);
            ps.prefer_input_pin_order = true;
        }
        catch (const std::invalid_argument& e)
        {
            env->out() << fmt::format("[w] {}\n", e.what());
            reset_command_state();
            return;
        }
    }

    if (is_set("output_pin_order"))
    {
        try
        {
            ps.output_pin_order        = parse_output_pin_order_csv(output_pin_order);
            ps.prefer_output_pin_order = true;
        }
        catch (const std::invalid_argument& e)
        {
            env->out() << fmt::format("[w] {}\n", e.what());
            reset_command_state();
            return;
        }
    }

    if (grid == "hex")
    {
        graph_oriented_layout_design_hex();
    }
    else if (grid == "cartesian")
    {
        graph_oriented_layout_design<fiction::cart_gate_clk_lyt>();
    }
    else
    {
        env->out() << "[w] unsupported grid; use 'cartesian' or 'hex'\n";
        reset_command_state();
        return;
    }

    reset_command_state();
}

nlohmann::json gold_command::log() const
{
    return nlohmann::json{{"runtime in seconds", mockturtle::to_seconds(st.time_total)},
                          {"number of gates", st.num_gates},
                          {"number of wires", st.num_wires},
                          {"number of crossings", st.num_crossings},
                          {"max placed nodes", st.max_placed_nodes},
                          {"num search space graphs", st.num_search_space_graphs},
                          {"zero candidate pis", st.zero_candidate_pis},
                          {"zero candidate gates", st.zero_candidate_gates},
                          {"zero candidate pos", st.zero_candidate_pos},
                          {"route failures gates", st.route_failures_gates},
                          {"route failures pos", st.route_failures_pos},
                          {"invalid layout prunes", st.invalid_layout_prunes},
                          {"deepest failed node",
                           {{"index", st.deepest_failed_node_index},
                            {"placed nodes", st.deepest_failed_placed_nodes},
                            {"fanin count", st.deepest_failed_fanin_count},
                            {"kind", st.deepest_failed_node_kind},
                            {"function", st.deepest_failed_node_function},
                            {"reason", st.deepest_failed_reason},
                            {"detail", st.deepest_failed_detail},
                            {"frontier kind", st.deepest_failed_frontier_kind},
                            {"frontier function", st.deepest_failed_frontier_function},
                            {"frontier pi run length", st.deepest_failed_frontier_pi_run_length},
                            {"driver kind", st.deepest_failed_driver_kind},
                            {"driver function", st.deepest_failed_driver_function},
                            {"placed successors", st.deepest_failed_placed_successors},
                            {"total successors", st.deepest_failed_total_successors},
                            {"ssg", st.deepest_failed_ssg}}},
                          {"layout", {{"x-size", st.x_size}, {"y-size", st.y_size}, {"area", st.x_size * st.y_size}}}};
}

template <typename Lyt>
void gold_command::graph_oriented_layout_design()
{
    const auto get_name = [](auto&& ntk_ptr) -> std::string { return fiction::get_name(*ntk_ptr); };

    const auto perform_physical_design = [this](auto&& ntk_ptr)
    { return fiction::graph_oriented_layout_design<Lyt>(*ntk_ptr, ps, &st); };

    const auto& ntk_ptr = store<fiction::logic_network_t>().current();

    try
    {
        const auto lyt = std::visit(perform_physical_design, ntk_ptr);

        if (lyt.has_value())
        {
            store<fiction::gate_layout_t>().extend() = std::make_shared<Lyt>(*lyt);
        }
        else
        {
            env->out() << fmt::format("[e] impossible to place and route '{}' within the given parameters\n",
                                      std::visit(get_name, ntk_ptr));
        }
    }
    catch (const fiction::high_degree_fanin_exception& e)
    {
        env->out() << fmt::format("[e] {}\n", e.what());
    }
    catch (const std::invalid_argument& e)
    {
        env->out() << fmt::format("[e] {}\n", e.what());
    }
    catch (...)
    {
        env->out() << fmt::format("[e] an error occurred while placing and routing '{}' with the given parameters\n",
                                  std::visit(get_name, ntk_ptr));
    }
}

void gold_command::graph_oriented_layout_design_hex()
{
    const auto get_name = [](auto&& ntk_ptr) -> std::string { return fiction::get_name(*ntk_ptr); };

    const auto perform_physical_design = [this](auto&& ntk_ptr)
    { return fiction::graph_oriented_layout_design_hex<fiction::hex_even_row_gate_clk_lyt>(*ntk_ptr, ps, &st); };

    const auto& ntk_ptr = store<fiction::logic_network_t>().current();

    try
    {
        const auto hex_lyt = std::visit(perform_physical_design, ntk_ptr);

        if (!hex_lyt.has_value())
        {
            env->out() << fmt::format("[e] impossible to place and route '{}' within the given parameters\n",
                                      std::visit(get_name, ntk_ptr));
            return;
        }

        store<fiction::gate_layout_t>().extend() = std::make_shared<fiction::hex_even_row_gate_clk_lyt>(*hex_lyt);
    }
    catch (const fiction::high_degree_fanin_exception& e)
    {
        env->out() << fmt::format("[e] {}\n", e.what());
    }
    catch (const std::invalid_argument& e)
    {
        env->out() << fmt::format("[e] {}\n", e.what());
    }
    catch (...)
    {
        env->out() << fmt::format("[e] an error occurred while placing and routing '{}' with the given parameters\n",
                                  std::visit(get_name, ntk_ptr));
    }
}

}  // namespace alice
