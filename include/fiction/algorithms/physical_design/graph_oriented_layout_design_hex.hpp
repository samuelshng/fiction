//
// Created by codex on 06.02.26.
//

#ifndef FICTION_GRAPH_ORIENTED_LAYOUT_DESIGN_HEX_HPP
#define FICTION_GRAPH_ORIENTED_LAYOUT_DESIGN_HEX_HPP

#include "fiction/algorithms/physical_design/graph_oriented_layout_design.hpp"

namespace fiction
{

namespace detail
{
/**
 * Implementation of the native hexagonal graph-oriented layout design algorithm.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @tparam Ntk Network type.
 */
template <typename Lyt, typename Ntk>
class graph_oriented_layout_design_hex_impl
{
  public:
    /**
     * Constructor for the graph-oriented layout design algorithm.
     *
     * @param src The source network to be placed.
     * @param p The parameters for the graph-enhanced layout search algorithm.
     * @param st The statistics object to record execution details.
     */
    graph_oriented_layout_design_hex_impl(const Ntk& src, const graph_oriented_layout_design_params& p,
                                          graph_oriented_layout_design_stats&       st,
                                          const std::function<uint64_t(const Lyt&)> custom) :
            ntk{initialize_network(src)},
            ps{p},
            pst{st},
            custom_cost_objective{custom},
            timeout{ps.timeout},
            start{std::chrono::high_resolution_clock::now()},
            seed{p.seed.value_or(std::random_device{}())}
    {
        ntk.substitute_po_signals();
        initialize_input_pin_order_ranks();
        initialize_output_pin_order_ranks();
    }
    /**
     * Executes the graph-oriented layout design algorithm and returns the best found layout.
     *
     * @return The best layout found by the algorithm.
     */
    std::optional<Lyt> run() noexcept
    {
        // measure run time
        mockturtle::stopwatch stop{pst.time_total};

        // calculate number of search space graphs
        num_search_space_graphs = calculate_num_search_space_graphs(ps.mode, ps.cost);

        // resize ssg_vec based on the new number of search space graphs
        ssg_vec.resize(num_search_space_graphs);

        // initialize layout to keep track of current best solution
        Lyt best_lyt{{}, row_clocking<Lyt>()};

        // initialize search space graphs
        initialize();

        // check if a timeout was set
        const bool timeout_set = (timeout != std::numeric_limits<uint64_t>::max());

        // if no timeout was set and high-effort mode is disabled, set timeout to 10s
        if (!timeout_set && (ps.mode == graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY))
        {
            timeout = 10000u;
        }

        // main loop
        while (!timeout_limit_reached)
        {
            // if multithreading is enabled
            if (ps.enable_multithreading)
            {
                // separate mutexes for better concurrency
                std::mutex update_best_layout_mutex{};

                // reuse futures pool to avoid allocation overhead
                futures_pool.clear();
                futures_pool.reserve(ssg_vec.size());

                // process `ssg_vec` in parallel using std::async
                for (auto& ssg : ssg_vec)
                {
                    auto* ssg_ptr = &ssg;
                    futures_pool.emplace_back(
                        std::async(std::launch::async,
                                   [this, ssg_ptr, &update_best_layout_mutex, &best_lyt]() -> std::optional<Lyt>
                                   {
                                       if (auto result = process_ssg(*ssg_ptr); result)
                                       {
                                           const std::lock_guard<std::mutex> lock(update_best_layout_mutex);
                                           best_lyt = std::move(*result);
                                           restore_names(ssg_ptr->network, best_lyt);
                                           update_stats(best_lyt);

                                           if (ps.return_first)
                                           {
                                               return best_lyt;
                                           }
                                       }
                                       return std::nullopt;
                                   }));
                }

                // check the futures for the result - poll for readiness to support return_first
                while (true)
                {
                    bool all_done = true;
                    for (auto& f : futures_pool)
                    {
                        if (f.valid())
                        {
                            using namespace std::chrono_literals;
                            if (f.wait_for(0ms) == std::future_status::ready)
                            {
                                if (auto r = f.get())
                                {
                                    // cancel remaining futures if return_first is enabled
                                    if (ps.return_first)
                                    {
                                        for (auto& remaining_future : futures_pool)
                                        {
                                            if (remaining_future.valid())
                                            {
                                                remaining_future.wait();
                                            }
                                        }
                                    }
                                    return *r;  // return immediately when first result is ready
                                }
                            }
                            else
                            {
                                all_done = false;
                            }
                        }
                    }
                    if (all_done)
                    {
                        break;
                    }
                    // adaptive sleep: shorter sleep when more futures are active
                    const auto active_futures = std::count_if(futures_pool.cbegin(), futures_pool.cend(),
                                                              [](const auto& f) { return f.valid(); });
                    const auto sleep_duration =
                        active_futures > 4 ? std::chrono::microseconds(100) : std::chrono::milliseconds(1);
                    std::this_thread::sleep_for(sleep_duration);
                }
            }
            else
            {
                // single-threaded version
                for (auto& ssg : ssg_vec)
                {
                    auto result = process_ssg(ssg);
                    if (result)
                    {
                        best_lyt = *result;
                        restore_names(ssg.network, best_lyt);
                        update_stats(best_lyt);

                        if (ps.return_first)
                        {
                            return *result;
                        }
                    }
                }
            }

            // update current_vertex and frontier_flag (non-parallel for now)
            for (auto& ssg : ssg_vec)
            {
                if (ssg.frontier_flag)
                {
                    if (!ssg.frontier.empty())
                    {
                        ssg.current_vertex = ssg.frontier.get();
                    }
                    else
                    {
                        ssg.frontier_flag = false;
                    }
                }
            }

            // check if timeout is reached or solution found
            timeout_limit_reached =
                std::none_of(ssg_vec.cbegin(), ssg_vec.cend(), [](const auto& ssg) { return ssg.frontier_flag; });

            const auto end = std::chrono::high_resolution_clock::now();
            const auto duration_ms =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

            if (duration_ms >= timeout)
            {
                // terminate the algorithm if the specified timeout was set or a solution was found in high-efficiency
                // mode
                if ((ps.mode == graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY &&
                     (improve_area_solution || improve_wire_solution || improve_crossing_solution ||
                      improve_acp_solution || improve_custom_solution)) ||
                    timeout_set)
                {
                    timeout_limit_reached = true;
                }
            }
        }

        // check if any layout was found
        if (improve_area_solution || improve_wire_solution || improve_crossing_solution || improve_acp_solution ||
            improve_custom_solution)
        {
            return best_lyt;
        }
        return std::nullopt;
    }

  private:
    /**
     * Alias for an obstruction layout based on the given layout type.
     */
    using ObstrLyt = obstruction_layout<Lyt>;
    /**
     * The network to be placed and routed.
     */
    tec_nt ntk;

    [[nodiscard]] static tec_nt initialize_network(const Ntk& src)
    {
        if constexpr (std::is_same_v<Ntk, tec_nt>)
        {
            return src;
        }

        return convert_network<tec_nt>(src);
    }
    /**
     * Parameters.
     */
    graph_oriented_layout_design_params ps;
    /**
     * Statistics.
     */
    graph_oriented_layout_design_stats& pst;
    /**
     * Custom cost objective.
     */
    const std::function<uint64_t(const Lyt&)> custom_cost_objective;
    /**
     * Timeout limit (in ms).
     */
    uint64_t timeout;
    /**
     * Timeout limit reached.
     */
    bool timeout_limit_reached = false;
    /**
     * Start time.
     */
    std::chrono::time_point<std::chrono::high_resolution_clock> start;
    /**
     * Number of search space graphs.
     */
    uint64_t num_search_space_graphs{0};
    /**
     * Vector of search space graphs.
     */
    std::vector<search_space_graph<ObstrLyt>> ssg_vec;
    /**
     * Keep track of the maximum number of placed nodes.
     */
    std::atomic<uint64_t> max_placed_nodes{0ul};
    /**
     * The current best solution with respect to area, initialized to the maximum possible value.
     * This value will be updated as better solutions are found.
     */
    std::atomic<uint64_t> best_area_solution = std::numeric_limits<uint64_t>::max();
    /**
     * The current best solution with respect to the number of wire segments, initialized to the maximum possible value.
     * This value will be updated as better solutions are found.
     */
    std::atomic<uint64_t> best_wire_solution = std::numeric_limits<uint64_t>::max();
    /**
     * The current best solution with respect to the number of crossings, initialized to the maximum possible
     * value. This value will be updated as better solutions are found.
     */
    std::atomic<uint64_t> best_crossing_solution = std::numeric_limits<uint64_t>::max();
    /**
     * The current best solution with respect to the area-crossing product (ACP), initialized to the maximum possible
     * value. This value will be updated as better solutions are found.
     */
    std::atomic<uint64_t> best_acp_solution = std::numeric_limits<uint64_t>::max();
    /**
     * The current best solution with respect to a custom cost objective, initialized to the maximum possible value.
     * This value will be updated as better solutions are found.
     */
    std::atomic<uint64_t> best_custom_solution = std::numeric_limits<uint64_t>::max();
    /**
     * Current best solution w.r.t. area after relocating POs.
     */
    std::atomic<uint64_t> best_optimized_solution{std::numeric_limits<uint64_t>::max()};
    /**
     * Flag indicating that an initial solution has been found with the layout area as cost objective.
     * When set to `true`, subsequent search space graphs with the layout area as cost objective can be pruned.
     */
    std::atomic<bool> improve_area_solution = false;
    /**
     * Flag indicating that an initial solution has been found with the number of wire segments as cost objective.
     * When set to `true`, subsequent search space graphs with the number of wire segments as cost objective can be
     * pruned.
     */
    std::atomic<bool> improve_wire_solution = false;
    /**
     * Flag indicating that an initial solution has been found with the number of crossings as cost objective.
     * When set to `true`, subsequent search space graphs with the number of crossings as cost objective can be pruned.
     */
    std::atomic<bool> improve_crossing_solution = false;
    /**
     * Flag indicating that an initial solution has been found with the area-crossings product as cost objective.
     * When set to `true`, subsequent search space graphs with the area-crossing product as cost objective can be
     * pruned.
     */
    std::atomic<bool> improve_acp_solution = false;
    /**
     * Flag indicating that an initial solution has been found with a custom cost objective.
     * When set to `true`, subsequent search space graphs with a custom cost objective can be pruned.
     */
    std::atomic<bool> improve_custom_solution = false;
    /**
     * In high-efficiency mode, only 1 search space graph is used.
     *
     * In native hex mode, PIs are fixed to the top border. Therefore, no additional PI-location variants are needed.
     */
    const uint64_t num_search_space_graphs_high_efficiency = 1u;
    /**
     * In high-effort mode, 4 search space graphs are used:
     * 1 (possible PI location: top) * 2 (fanout substitution strategies) * 2 (topological orderings).
     */
    const uint64_t num_search_space_graphs_high_effort = 4u;
    /**
     * In highest-effort mode, 16 search space graphs are used.
     *
     * This includes 4 search space graphs for each of the four base cost objectives layout area, number of wire
     * segments, number of wire crossings, and area-crossing product.
     */
    const uint64_t num_search_space_graphs_highest_effort = 4u * num_search_space_graphs_high_effort;
    /**
     * In maximum-effort mode, 32 search space graphs are used.
     *
     * It adds another 16 search space graphs to the 16 search space graphs from highest-effort mode using randomized
     * fanout substitution strategies and random topological orderings.
     */
    const uint64_t num_search_space_graphs_maximum_effort = 2u * num_search_space_graphs_highest_effort;
    /**
     * In highest-effort mode with a custom cost function, 20 search space graphs are used
     * (16 with the standard cost objectives and 4 for the custom one).
     */
    const uint64_t num_search_space_graphs_highest_effort_custom = 5u * num_search_space_graphs_high_effort;
    /**
     * In maximum-effort mode with a custom cost function, 40 search space graphs are used
     * (32 with the standard cost objectives and 8 for the custom one).
     */
    const uint64_t num_search_space_graphs_maximum_effort_custom = 2u * num_search_space_graphs_highest_effort_custom;
    /**
     * Random seed used for random fanout substitution and random topological ordering in maximum-effort mode.
     */
    std::uint32_t seed;
    /**
     * Primary input ranks by declaration index used for deterministic PI reordering.
     *
     * The value at index `i` stores the preferred rank of the `i`-th PI in declaration order.
     */
    std::vector<uint64_t> input_pin_order_ranks{};
    /**
     * Primary output ranks by declaration index used for deterministic PO reordering.
     *
     * The value at index `i` stores the preferred rank of the `i`-th PO in declaration order.
     */
    std::vector<uint64_t> output_pin_order_ranks{};
    /**
     * Explicit primary output ranks by PO name.
     *
     * This map is populated only if an explicit `output_pin_order` list is provided.
     */
    std::unordered_map<std::string, uint64_t> output_pin_order_ranks_by_name{};
    /**
     * Thread pool for multithreaded execution to avoid thread creation overhead.
     */
    mutable std::vector<std::future<std::optional<Lyt>>> futures_pool{};
    /**
     * Initializes PI order ranks from user parameters.
     *
     * If `prefer_input_pin_order` is enabled without an explicit PI order list, declaration order is used.
     * If an explicit order list is provided, it is validated against network PI names and converted into ranks.
     *
     * @throws std::invalid_argument If the provided PI order list is invalid.
     */
    void initialize_input_pin_order_ranks()
    {
        if (!ps.prefer_input_pin_order)
        {
            return;
        }

        const auto num_pis = ntk.num_pis();

        input_pin_order_ranks.resize(num_pis);
        for (uint64_t i = 0u; i < num_pis; ++i)
        {
            input_pin_order_ranks[i] = i;
        }

        if (ps.input_pin_order.empty())
        {
            return;
        }

        if (ps.input_pin_order.size() != num_pis)
        {
            throw std::invalid_argument(fmt::format("PI order list size ({}) does not match the number of PIs ({}).",
                                                    ps.input_pin_order.size(), num_pis));
        }

        std::unordered_map<std::string, uint64_t> declaration_index_by_name{};
        declaration_index_by_name.reserve(num_pis);

        uint64_t declaration_index = 0u;
        ntk.foreach_pi(
            [this, &declaration_index_by_name, &declaration_index](const auto& pi)
            {
                const auto pi_signal = ntk.make_signal(pi);

                if (!ntk.has_name(pi_signal))
                {
                    throw std::invalid_argument(
                        "Explicit PI ordering requires named PIs, but at least one PI has no name.");
                }

                const auto pi_name = ntk.get_name(pi_signal);

                if (const auto [_, inserted] = declaration_index_by_name.emplace(pi_name, declaration_index); !inserted)
                {
                    throw std::invalid_argument(
                        fmt::format("PI names must be unique for explicit ordering. Duplicate name: '{}'.", pi_name));
                }

                ++declaration_index;
            });

        std::fill(input_pin_order_ranks.begin(), input_pin_order_ranks.end(), num_pis);

        uint64_t preferred_rank = 0u;
        for (const auto& requested_name : ps.input_pin_order)
        {
            if (requested_name.empty())
            {
                throw std::invalid_argument("PI order list contains an empty PI name.");
            }

            const auto it = declaration_index_by_name.find(requested_name);
            if (it == declaration_index_by_name.cend())
            {
                throw std::invalid_argument(
                    fmt::format("PI order list contains unknown PI name '{}'.", requested_name));
            }

            auto& rank_slot = input_pin_order_ranks[it->second];
            if (rank_slot != num_pis)
            {
                throw std::invalid_argument(
                    fmt::format("PI order list contains duplicate PI name '{}'.", requested_name));
            }

            rank_slot = preferred_rank++;
        }
    }
    /**
     * Initializes PO order ranks from user parameters.
     *
     * If `prefer_output_pin_order` is enabled without an explicit PO order list, declaration order is used.
     * If an explicit order list is provided, it is validated against network PO names and converted into ranks.
     *
     * @throws std::invalid_argument If the provided PO order list is invalid.
     */
    void initialize_output_pin_order_ranks()
    {
        output_pin_order_ranks_by_name.clear();

        if (!ps.prefer_output_pin_order)
        {
            return;
        }

        const auto num_pos = ntk.num_pos();

        output_pin_order_ranks.resize(num_pos);
        for (uint64_t i = 0u; i < num_pos; ++i)
        {
            output_pin_order_ranks[i] = i;
        }

        if (ps.output_pin_order.empty())
        {
            return;
        }

        if (ps.output_pin_order.size() != num_pos)
        {
            throw std::invalid_argument(fmt::format("PO order list size ({}) does not match the number of POs ({}).",
                                                    ps.output_pin_order.size(), num_pos));
        }

        std::unordered_map<std::string, uint64_t> declaration_index_by_name{};
        declaration_index_by_name.reserve(num_pos);

        ntk.foreach_po(
            [this, &declaration_index_by_name](const auto&, const auto i)
            {
                if (!ntk.has_output_name(i))
                {
                    throw std::invalid_argument(
                        "Explicit PO ordering requires named POs, but at least one PO has no name.");
                }

                const auto po_name = ntk.get_output_name(i);

                if (const auto [_, inserted] = declaration_index_by_name.emplace(po_name, i); !inserted)
                {
                    throw std::invalid_argument(
                        fmt::format("PO names must be unique for explicit ordering. Duplicate name: '{}'.", po_name));
                }
            });

        std::fill(output_pin_order_ranks.begin(), output_pin_order_ranks.end(), num_pos);
        output_pin_order_ranks_by_name.reserve(num_pos);

        uint64_t preferred_rank = 0u;
        for (const auto& requested_name : ps.output_pin_order)
        {
            if (requested_name.empty())
            {
                throw std::invalid_argument("PO order list contains an empty PO name.");
            }

            const auto it = declaration_index_by_name.find(requested_name);
            if (it == declaration_index_by_name.cend())
            {
                throw std::invalid_argument(
                    fmt::format("PO order list contains unknown PO name '{}'.", requested_name));
            }

            auto& rank_slot = output_pin_order_ranks[it->second];
            if (rank_slot != num_pos)
            {
                throw std::invalid_argument(
                    fmt::format("PO order list contains duplicate PO name '{}'.", requested_name));
            }

            rank_slot = preferred_rank;
            output_pin_order_ranks_by_name.emplace(requested_name, preferred_rank);
            ++preferred_rank;
        }
    }
    /**
     * Get thread-local random number generator for `tiles_to_skip_between_pis` randomization.
     * Each thread will have its own RNG to avoid mutex contention.
     *
     * @return Reference to a thread-local Mersenne Twister random number generator.
     */
    [[nodiscard]] std::mt19937& get_thread_local_rng() const
    {
        thread_local std::mt19937 rng{seed};
        return rng;
    }
    /**
     * Get thread-local distribution for generating random `tiles_to_skip_between_pis` values.
     *
     * @return Reference to a thread-local uniform integer distribution for generating random skip values.
     */
    [[nodiscard]] std::uniform_int_distribution<uint64_t>& get_thread_local_dist() const
    {
        // Handle edge case where tiles_to_skip_between_pis is 0
        const auto min_val = ps.tiles_to_skip_between_pis > 0 ? ps.tiles_to_skip_between_pis - 1 : 0;
        thread_local std::uniform_int_distribution<uint64_t> dist{min_val, ps.tiles_to_skip_between_pis};
        return dist;
    }
    /**
     * Determines the number of search space graphs to generate based on the selected effort mode and cost objective.
     *
     * @param mode The effort mode chosen for the layout design, determining the level of computational effort.
     * @param cost The cost that specifies the optimization objective for the layout design.
     * @return The number of search space graphs to be generated.
     */
    [[nodiscard]] std::uint64_t
    calculate_num_search_space_graphs(graph_oriented_layout_design_params::effort_mode    mode,
                                      graph_oriented_layout_design_params::cost_objective cost) noexcept
    {
        if (mode == graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT)
        {
            return (cost == graph_oriented_layout_design_params::cost_objective::CUSTOM) ?
                       num_search_space_graphs_maximum_effort_custom :
                       num_search_space_graphs_maximum_effort;
        }
        if (mode == graph_oriented_layout_design_params::effort_mode::HIGHEST_EFFORT)
        {
            return (cost == graph_oriented_layout_design_params::cost_objective::CUSTOM) ?
                       num_search_space_graphs_highest_effort_custom :
                       num_search_space_graphs_highest_effort;
        }
        return (mode == graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT) ?
                   num_search_space_graphs_high_effort :
                   num_search_space_graphs_high_efficiency;
    }
    /**
     * This function updates statistical metrics.
     *
     * @param best_lyt The new best layout found.
     */
    void update_stats(const Lyt& best_lyt)
    {
        // Statistical information
        pst.x_size        = best_lyt.x() + 1;
        pst.y_size        = best_lyt.y() + 1;
        pst.num_gates     = best_lyt.num_gates();
        pst.num_wires     = best_lyt.num_wires();
        pst.num_crossings = best_lyt.num_crossings();
    }
    /**
     * Checks if there is a path between the source and destination tiles in the given layout.
     *
     * @param layout The layout to be checked.
     * @param src The source tile.
     * @param dest The destination tile.
     * @param new_gate_loc Enum indicating if the src or dest have to host a new gate and therefore have to be empty.
     * Defaults to `new_gate_location::NONE`.
     * @return A path from `src` to `dest` if one exists.
     */
    [[nodiscard]] layout_coordinate_path<ObstrLyt>
    check_path(const ObstrLyt& layout, const tile<ObstrLyt>& src, const tile<ObstrLyt>& dest,
               const new_gate_location new_gate_loc            = new_gate_location::NONE,
               const bool              check_straight_inverter = false) noexcept
    {
        const bool src_is_new_pos  = (new_gate_loc == new_gate_location::SRC);
        const bool dest_is_new_pos = (new_gate_loc == new_gate_location::DEST);

        if ((layout.is_empty_tile(src) && src_is_new_pos) || (layout.is_empty_tile(dest) && dest_is_new_pos) ||
            (new_gate_loc == new_gate_location::NONE))
        {
            using dist = manhattan_distance_functor<ObstrLyt, uint64_t>;
            using cost = unit_cost_functor<ObstrLyt, uint8_t>;

            a_star_params a_star_crossing_params{};
            a_star_crossing_params.crossings = !ps.planar;

            const auto path =
                a_star<layout_coordinate_path<ObstrLyt>>(layout, {src, dest}, dist(), cost(), a_star_crossing_params);

            if (path.size() < 2)
            {
                return {};
            }

            if (!check_straight_inverter)
            {
                return path;
            }

            const auto fanin                        = layout.incoming_data_flow(src).front();
            const bool vertical_straight_inverter   = (fanin.x == src.x && src.x == path[1].x);
            const bool horizontal_straight_inverter = (fanin.y == src.y && src.y == path[1].y);
            const bool straight_inverter            = vertical_straight_inverter || horizontal_straight_inverter;

            if (straight_inverter)
            {
                return path;
            }
        }

        return {};
    }
    /**
     * Retrieves the possible positions for Primary Inputs (PIs) in the given layout based on the specified
     * criteria of positioning at the top or left side, with a limit on the number of possible positions.
     *
     * @param layout The layout in which to find the possible positions for PIs.
     * @param pi_locs Struct indicating if PIs are allowed at the top or left side of the layout.
     * @param num_expansions The maximum number of positions to be returned (is doubled for PIs).
     * @return A vector of tiles representing the possible positions for PIs.
     */
    [[nodiscard]] bool has_path_to_bottom_row(const ObstrLyt& layout, const tile<ObstrLyt>& src,
                                              const new_gate_location new_gate_loc            = new_gate_location::NONE,
                                              const bool              check_straight_inverter = false) noexcept
    {
        for (uint64_t x = 0u; x <= layout.x(); ++x)
        {
            if (!check_path(layout, src, {x, layout.y(), 0}, new_gate_loc, check_straight_inverter).empty())
            {
                return true;
            }
        }

        return false;
    }
    [[nodiscard]] coord_vec_type<ObstrLyt> get_possible_positions_pis(ObstrLyt& layout, const pi_locations& pi_locs,
                                                                      const uint64_t num_expansions) noexcept
    {
        uint64_t count_expansions = 0ul;

        coord_vec_type<ObstrLyt> possible_positions{};

        // if no PIs yet, no skipping; otherwise use appropriate setting.
        uint64_t skip_tiles = 0;
        if (!layout.is_empty())
        {
            if (ps.randomize_tiles_to_skip_between_pis)
            {
                // generate random skip_tiles for each PI placement using thread-local RNG
                skip_tiles = get_thread_local_dist()(get_thread_local_rng());
            }
            else
            {
                skip_tiles = ps.tiles_to_skip_between_pis;
            }
        }
        auto skip_top  = skip_tiles;
        auto skip_left = skip_tiles;

        // make sure we have enough margin in both directions.
        const uint64_t resize = skip_tiles + 1;

        layout.resize({layout.x() + resize, layout.y() + resize, layout.z()});
        const tile<ObstrLyt> drain{layout.x(), layout.y(), 0};

        uint64_t min_x = 0;
        uint64_t min_y = 0;

        if (skip_tiles != 0)
        {
            for (auto x = static_cast<int64_t>(layout.x()); x >= 0; --x)
            {
                if (!layout.is_empty_tile({static_cast<uint64_t>(x), 0, 0}))
                {
                    min_x = static_cast<uint64_t>(x) + 1;
                    break;  // first non-empty from the right
                }
            }

            for (auto y = static_cast<int64_t>(layout.y()); y >= 0; --y)
            {
                if (!layout.is_empty_tile({0, static_cast<uint64_t>(y), 0}))
                {
                    min_y = static_cast<uint64_t>(y) + 1;
                    break;  // first non-empty from the bottom
                }
            }
        }

        // check if a path from the input to the drain exists
        const auto check_tile = [&](const uint64_t x, const uint64_t y) noexcept
        {
            const tile<ObstrLyt> tile{x, y, 0};
            if constexpr (is_hexagonal_layout_v<ObstrLyt>)
            {
                if (layout.is_empty_tile(tile))
                {
                    count_expansions++;
                    possible_positions.push_back(tile);
                }
            }
            else if (!check_path(layout, tile, drain, new_gate_location::SRC).empty())
            {
                count_expansions++;
                possible_positions.push_back(tile);
            }
        };

        uint64_t max_iterations = 0;

        if (pi_locs == pi_locations::TOP_AND_LEFT)
        {
            max_iterations = std::max(layout.x() - min_x, layout.y() - min_y);
        }
        else if (pi_locs == pi_locations::TOP)
        {
            max_iterations = layout.x() - min_x;
        }
        else
        {
            max_iterations = layout.y() - min_y;
        }

        uint64_t expansion_limit = (pi_locs == pi_locations::TOP_AND_LEFT) ? 2 * num_expansions : num_expansions;
        if constexpr (is_hexagonal_layout_v<ObstrLyt>)
        {
            if (ps.prefer_input_pin_order && (pi_locs == pi_locations::TOP || pi_locs == pi_locations::TOP_AND_LEFT))
            {
                expansion_limit = std::max(expansion_limit, layout.x() + 1);
            }
        }

        possible_positions.reserve(expansion_limit);

        for (uint64_t k = 0ul; k < max_iterations; k++)
        {
            if (((pi_locs == pi_locations::TOP) || (pi_locs == pi_locations::TOP_AND_LEFT)) && min_x + k < layout.x())
            {
                if (skip_top != 0)
                {
                    --skip_top;
                }
                else
                {
                    check_tile(min_x + k, 0);
                }
            }
            if (((pi_locs == pi_locations::LEFT) || (pi_locs == pi_locations::TOP_AND_LEFT)) && min_y + k < layout.y())
            {
                if (skip_left == 0)
                {
                    check_tile(0, min_y + k);
                }
                else
                {
                    --skip_left;
                }
            }
            if (count_expansions >= expansion_limit)
            {
                layout.resize({layout.x() - resize, layout.y() - resize, layout.z()});

                return possible_positions;
            }
        }

        layout.resize({layout.x() - resize, layout.y() - resize, layout.z()});

        return possible_positions;
    }
    /**
     * Retrieves the possible positions for Primary Outputs (POs) in the given layout based on the positions
     * of the preceding nodes.
     *
     * @param layout The layout in which to find the possible positions for POs.
     * @param place_info The placement context containing current node, primary output index, node to position mapping,
     * and PI to node mapping.
     * @param fc A vector of nodes that precede the PO nodes.
     * @return A vector of tiles representing the possible positions for POs.
     */
    [[nodiscard]] coord_vec_type<ObstrLyt> get_possible_positions_pos(const ObstrLyt&                 layout,
                                                                      const placement_info<ObstrLyt>& place_info,
                                                                      const fanin_container<tec_nt>&  fc) noexcept
    {
        coord_vec_type<ObstrLyt> possible_positions{};

        const auto& pre   = fc.fanin_nodes[0];
        const auto  pre_t = static_cast<tile<ObstrLyt>>(place_info.node2pos[pre]);

        if constexpr (is_hexagonal_layout_v<ObstrLyt>)
        {
            possible_positions.reserve(layout.x() + 1);
        }
        else
        {
            const auto expansion_limit = std::max(layout.x() - pre_t.x, layout.y() - pre_t.y);
            possible_positions.reserve(expansion_limit);
        }

        // check if path from previous tile to PO exists
        auto check_tile = [&](const uint64_t x, const uint64_t y)
        {
            const tile<ObstrLyt> tile{x, y, 0};
            const auto check_straight_inverter = ps.straight_inverters && layout.is_inv(layout.get_node(pre_t));

            if (!check_path(layout, pre_t, tile, new_gate_location::DEST, check_straight_inverter).empty())
            {
                possible_positions.push_back(tile);
            }
        };

        if constexpr (is_hexagonal_layout_v<ObstrLyt>)
        {
            for (uint64_t x = 0u; x <= layout.x(); ++x)
            {
                check_tile(x, layout.y());
            }
            return possible_positions;
        }

        const auto expansion_limit = std::max(layout.x() - pre_t.x, layout.y() - pre_t.y);
        for (uint64_t k = 0ul; k <= expansion_limit; ++k)
        {
            if (pre_t.x + k <= layout.x())
            {
                check_tile(pre_t.x + k, layout.y());
            }
            if constexpr (!is_hexagonal_layout_v<ObstrLyt>)
            {
                if (pre_t.y + k < layout.y())
                {
                    check_tile(layout.x(), pre_t.y + k);
                }
            }
        }

        return possible_positions;
    }
    /**
     * Retrieves the possible positions for a single fan-in node in the given layout, based on the positions
     * of preceding nodes and a specified number of expansions.
     *
     * @param layout The layout in which to find the possible positions for a single fan-in node.
     * @param place_info The placement context containing current node, primary output index, node to position mapping,
     * and PI to node mapping.
     * @param fc A vector of nodes that precede the single fanin node.
     * @return A vector of tiles representing the possible positions for a single fan-in node.
     */
    [[nodiscard]] coord_vec_type<ObstrLyt>
    get_possible_positions_single_fanin(ObstrLyt& layout, const placement_info<ObstrLyt>& place_info,
                                        const fanin_container<tec_nt>& fc) noexcept
    {
        coord_vec_type<ObstrLyt> possible_positions{};
        possible_positions.reserve(ps.num_vertex_expansions);

        uint64_t count_expansions = 0ul;

        const auto& pre   = fc.fanin_nodes[0];
        const auto  pre_t = static_cast<tile<ObstrLyt>>(place_info.node2pos[pre]);

        // check if path from previous tile to new tile and from new tile to bottom row exists
        const auto check_tile = [&](const tile<ObstrLyt>& new_pos) noexcept
        {
            const auto check_straight_inverter = ps.straight_inverters && layout.is_inv(layout.get_node(pre_t));

            if (!check_path(layout, pre_t, new_pos, new_gate_location::DEST, check_straight_inverter).empty())
            {
                layout.resize({layout.x() + 1, layout.y() + 1, 1});
                if (has_path_to_bottom_row(layout, new_pos, new_gate_location::SRC))
                {
                    possible_positions.push_back(new_pos);
                    count_expansions++;
                }

                layout.resize({layout.x() - 1, layout.y() - 1, 1});
            }
        };

        if constexpr (is_hexagonal_layout_v<ObstrLyt>)
        {
            for (uint64_t y = pre_t.y + 1; y <= layout.y(); ++y)
            {
                for (uint64_t x = 0u; x <= layout.x(); ++x)
                {
                    check_tile({x, y, 0});
                    if (count_expansions >= ps.num_vertex_expansions)
                    {
                        return possible_positions;
                    }
                }
            }

            return possible_positions;
        }

        // iterate diagonally
        for (uint64_t k = 0ul; k < layout.x() + layout.y() + 1; ++k)
        {
            for (uint64_t x = 0ul; x < k + 1; ++x)
            {
                const auto y = k - x;
                if ((pre_t.y + y) <= layout.y() && (pre_t.x + x) <= layout.x())
                {
                    check_tile({pre_t.x + x, pre_t.y + y, 0});
                }
                if (count_expansions >= ps.num_vertex_expansions)
                {
                    return possible_positions;
                }
            }
        }

        return possible_positions;
    }
    /**
     * Retrieves the possible positions for a double fan-in node in the given layout, based on the positions
     * of preceding nodes and a specified number of expansions.
     *
     * @param layout The layout in which to find the possible positions for a double fan-in node.
     * @param place_info The placement context containing current node, primary output index, node to position mapping,
     * and PI to node mapping.
     * @param fc A vector of nodes that precede the double fanin node.
     * @return A vector of tiles representing the possible positions for a double fan-in node.
     */
    [[nodiscard]] coord_vec_type<ObstrLyt>
    get_possible_positions_double_fanin(ObstrLyt& layout, const placement_info<ObstrLyt>& place_info,
                                        const fanin_container<tec_nt>& fc) noexcept
    {
        coord_vec_type<ObstrLyt> possible_positions{};
        possible_positions.reserve(ps.num_vertex_expansions);
        uint64_t count_expansions = 0ul;

        const auto& pre1 = fc.fanin_nodes[0];
        const auto& pre2 = fc.fanin_nodes[1];

        const auto pre1_t = static_cast<tile<ObstrLyt>>(place_info.node2pos[pre1]);
        const auto pre2_t = static_cast<tile<ObstrLyt>>(place_info.node2pos[pre2]);

        const auto min_x = std::max(pre1_t.x, pre2_t.x) + (pre1_t.x == pre2_t.x ? 1 : 0);
        const auto min_y = std::max(pre1_t.y, pre2_t.y) + (pre1_t.y == pre2_t.y ? 1 : 0);

        // check if path from previous tiles to new tile and from new tile to bottom row exists
        auto check_tile = [&](const tile<ObstrLyt>& new_pos)
        {
            auto check_straight_inverter = ps.straight_inverters && layout.is_inv(layout.get_node(pre1_t));

            const auto path = check_path(layout, pre1_t, new_pos, new_gate_location::DEST, check_straight_inverter);
            if (!path.empty())
            {
                for (const auto& el : path)
                {
                    layout.obstruct_coordinate(el);
                }

                check_straight_inverter = ps.straight_inverters && layout.is_inv(layout.get_node(pre2_t));
                if (!check_path(layout, pre2_t, new_pos, new_gate_location::DEST, check_straight_inverter).empty())
                {
                    layout.resize({layout.x() + 1, layout.y() + 1, 1});
                    if (has_path_to_bottom_row(layout, new_pos, new_gate_location::SRC))
                    {
                        possible_positions.push_back(new_pos);
                        count_expansions++;
                    }

                    layout.resize({layout.x() - 1, layout.y() - 1, 1});
                }

                for (const auto& el : path)
                {
                    layout.clear_obstructed_coordinate(el);
                }
            }
        };

        if constexpr (is_hexagonal_layout_v<ObstrLyt>)
        {
            const auto min_hex_y = std::max(pre1_t.y, pre2_t.y) + 1;
            for (uint64_t y = min_hex_y; y <= layout.y(); ++y)
            {
                for (uint64_t x = 0u; x <= layout.x(); ++x)
                {
                    check_tile({x, y, 0});
                    if (count_expansions >= ps.num_vertex_expansions)
                    {
                        return possible_positions;
                    }
                }
            }

            return possible_positions;
        }

        // iterate diagonally
        for (uint64_t k = 0ul; k < layout.x() + layout.y() + 1; ++k)
        {
            for (uint64_t x = 0ul; x < k + 1; ++x)
            {
                const auto y = k - x;
                if ((min_y + y) <= layout.y() && (min_x + x) <= layout.x())
                {
                    check_tile({min_x + x, min_y + y, 0});
                }
                if (count_expansions >= ps.num_vertex_expansions)
                {
                    return possible_positions;
                }
            }
        }

        return possible_positions;
    }
    /**
     * Retrieves the possible positions for a given node in the layout based on its type and preceding nodes.
     * It determines the type of the node (PI, PO, single fan-in, double fan-in) and returns the corresponding
     * possible positions.
     *
     * @param layout The layout in which to find the possible positions.
     * @param place_info The placement context containing current node, primary output index, node to position mapping,
     * and PI to node mapping.
     * @param ssg The search space graph.
     * @return A vector of tiles representing the possible positions for the current node.
     */
    [[nodiscard]] coord_vec_type<ObstrLyt> get_possible_positions(ObstrLyt&                           layout,
                                                                  const search_space_graph<ObstrLyt>& ssg,
                                                                  const placement_info<ObstrLyt>& place_info) noexcept
    {
        const auto fc = fanins(ssg.network, ssg.nodes_to_place[place_info.current_node]);

        if (ssg.network.is_pi(ssg.nodes_to_place[place_info.current_node]))
        {
            return get_possible_positions_pis(layout, ssg.pi_locs, ssg.network.num_pis());
        }
        if (ssg.network.is_po(ssg.nodes_to_place[place_info.current_node]))
        {
            return get_possible_positions_pos(layout, place_info, fc);
        }
        if (fc.fanin_nodes.size() == 1)
        {
            return get_possible_positions_single_fanin(layout, place_info, fc);
        }

        return get_possible_positions_double_fanin(layout, place_info, fc);
    }
    /**
     * Validates the given layout based on the nodes in the network and their mappings in the node dictionary.
     * It checks if the placement of nodes in the layout is possible and ensures there are valid paths from each tile to
     * the drain.
     *
     * @param layout The layout to be validated.
     * @param place_info The placement context containing current node, primary output index, node to position mapping,
     * and PI to node mapping.
     * @param ssg The search space graph.
     */
    [[nodiscard]] bool valid_layout(ObstrLyt& layout, const search_space_graph<ObstrLyt>& ssg,
                                    const placement_info<ObstrLyt>& place_info) noexcept
    {
        if constexpr (is_hexagonal_layout_v<ObstrLyt>)
        {
            return true;
        }

        const auto check_tile = [&](const auto& t) noexcept
        {
            layout.resize({layout.x() + 1, layout.y() + 1, 1});
            const bool path_exists = has_path_to_bottom_row(layout, t, new_gate_location::DEST);
            layout.resize({layout.x() - 1, layout.y() - 1, 1});

            return path_exists;
        };

        const auto is_empty_tile_or_crossable = [&](const auto& t) noexcept
        {
            return layout.is_empty_tile(t) ||
                   (layout.is_empty_tile({t.x, t.y, 1}) && !layout.is_obstructed_coordinate({t.x, t.y, 1}));
        };

        for (uint64_t node = 0ul; node < place_info.current_node; node++)
        {
            const auto layout_tile = static_cast<tile<ObstrLyt>>(place_info.node2pos[ssg.nodes_to_place[node]]);
            const bool no_fanout_and_not_po =
                !layout.is_po_tile(layout_tile) && (layout.fanout_size(layout.get_node(layout_tile)) == 0);
            const bool one_dangling_fanout = (layout.fanout_size(layout.get_node(layout_tile)) == 1) &&
                                             ssg.network.is_fanout(ssg.nodes_to_place[node]);

            if (no_fanout_and_not_po || one_dangling_fanout)
            {
                if (!check_tile(layout_tile))
                {
                    return false;
                }

                const bool check_straight_inverter =
                    layout.is_inv(layout.get_node(layout_tile)) && ps.straight_inverters;

                if (check_straight_inverter)
                {
                    const tile<ObstrLyt> right_tile{layout_tile.x + 1, layout_tile.y, 0};
                    const tile<ObstrLyt> bottom_tile{layout_tile.x, layout_tile.y + 1, 0};

                    const auto fanin = layout.incoming_data_flow(layout_tile).front();
                    if ((fanin.x == layout_tile.x) && !is_empty_tile_or_crossable(bottom_tile))
                    {
                        return false;
                    }
                    if ((fanin.y == layout_tile.y) && !is_empty_tile_or_crossable(right_tile))
                    {
                        return false;
                    }
                }
            }

            const bool two_dangling_fanouts = (layout.fanout_size(layout.get_node(layout_tile)) == 0) &&
                                              ssg.network.is_fanout(ssg.nodes_to_place[node]);

            if (two_dangling_fanouts)
            {
                const tile<ObstrLyt> right_tile{layout_tile.x + 1, layout_tile.y, 0};
                const tile<ObstrLyt> bottom_tile{layout_tile.x, layout_tile.y + 1, 0};

                if (!(is_empty_tile_or_crossable(right_tile) && is_empty_tile_or_crossable(bottom_tile)))
                {
                    return false;
                }
            }
        }

        return true;
    }
    /**
     * Places a node with a single input in the layout and routes it.
     *
     * @param position The tile representing the position for placement.
     * @param layout The layout in which to place the node.
     * @param node2pos A dictionary mapping nodes from the network to signals in the layout.
     * @param fc A vector of nodes that precede the single fanin node.
     */
    void route_single_input_node(const tile<ObstrLyt>& position, ObstrLyt& layout,
                                 node_dict_type<ObstrLyt, tec_nt>& node2pos, const fanin_container<tec_nt>& fc) noexcept
    {
        const auto& pre  = fc.fanin_nodes[0];
        auto        src  = node2pos[pre];
        src.output       = fc.fanin_signals[0].output;
        const auto pre_t = static_cast<tile<ObstrLyt>>(src);

        layout.move_node(layout.get_node(position), position, {});

        const auto path = check_path(layout, pre_t, position, new_gate_location::NONE);
        assert(!path.empty());

        route_path(layout, src, path);

        for (const auto& el : path)
        {
            layout.obstruct_coordinate(el);
        }
    }
    /**
     * Places a node with two inputs in the layout and routes it.
     *
     * @param position The tile representing the position for placement.
     * @param layout The layout in which to place the node.
     * @param node2pos A dictionary mapping nodes from the network to signals in the layout.
     * @param fc A vector of nodes that precede the double fanin node.
     */
    void route_double_input_node(const tile<ObstrLyt>& position, ObstrLyt& layout,
                                 node_dict_type<ObstrLyt, tec_nt>& node2pos, const fanin_container<tec_nt>& fc) noexcept
    {
        const auto& pre1 = fc.fanin_nodes[0];
        const auto& pre2 = fc.fanin_nodes[1];

        auto src1   = node2pos[pre1];
        src1.output = fc.fanin_signals[0].output;
        auto src2   = node2pos[pre2];
        src2.output = fc.fanin_signals[1].output;

        const auto pre1_t = static_cast<tile<ObstrLyt>>(src1);
        const auto pre2_t = static_cast<tile<ObstrLyt>>(src2);

        layout.move_node(layout.get_node(position), position, {});

        const auto path_1 = check_path(layout, pre1_t, position, new_gate_location::NONE);
        assert(!path_1.empty());

        for (const auto& el : path_1)
        {
            layout.obstruct_coordinate(el);
        }

        const auto path_2 = check_path(layout, pre2_t, position, new_gate_location::NONE);
        assert(!path_2.empty());

        for (const auto& el : path_2)
        {
            layout.obstruct_coordinate(el);
        }

        route_path(layout, src1, path_1);
        route_path(layout, src2, path_2);
    }
    /**
     * Executes a single placement step in the layout for the given network node. It determines the type of the node,
     * places it accordingly, and checks if a solution was found.
     *
     * @param position The tile representing the position for placement.
     * @param layout The layout in which to place the node.
     * @param place_info The placement context containing current node, primary output index, node to position mapping,
     * and PI to node mapping.
     * @param ssg The search space graph.
     * @return A boolean indicating if a solution was found.
     */
    [[nodiscard]] bool place_and_route(const tile<ObstrLyt>& position, ObstrLyt& layout,
                                       search_space_graph<ObstrLyt>& ssg, placement_info<ObstrLyt>& place_info) noexcept
    {
        // vector to store preceding nodes
        const auto fc = fanins(ssg.network, ssg.nodes_to_place[place_info.current_node]);

        if (ssg.network.is_pi(ssg.nodes_to_place[place_info.current_node]))
        {
            if (position.x > layout.x())
            {
                layout.resize({position.x, layout.y(), layout.z()});
            }
            if (position.y > layout.y())
            {
                layout.resize({layout.x(), position.y, layout.z()});
            }
            // place primary input node
            place_info.node2pos[ssg.nodes_to_place[place_info.current_node]] =
                layout.move_node(place_info.pi2node[ssg.nodes_to_place[place_info.current_node]], position);
        }
        else if (fc.fanin_nodes.size() == 1)
        {
            const auto& pre = fc.fanin_nodes[0];
            auto        src = place_info.node2pos[pre];
            src.output      = fc.fanin_signals[0].output;

            // place single input node
            if (ssg.network.is_po(ssg.nodes_to_place[place_info.current_node]))
            {
                place_info.node2pos[ssg.nodes_to_place[place_info.current_node]] =
                    layout.create_po(src, fmt::format("po{}", place_info.current_po++), position);
            }
            else
            {
                place_info.node2pos[ssg.nodes_to_place[place_info.current_node]] =
                    place(layout, position, ssg.network, ssg.nodes_to_place[place_info.current_node], src);
            }

            route_single_input_node(position, layout, place_info.node2pos, fc);
        }
        else
        {
            // place double input node
            const auto& pre1 = fc.fanin_nodes[0];
            const auto& pre2 = fc.fanin_nodes[1];

            auto a1   = place_info.node2pos[pre1];
            a1.output = fc.fanin_signals[0].output;
            auto a2   = place_info.node2pos[pre2];
            a2.output = fc.fanin_signals[1].output;

            place_info.node2pos[ssg.nodes_to_place[place_info.current_node]] = place(
                layout, position, ssg.network, ssg.nodes_to_place[place_info.current_node], a1, a2, fc.constant_fanin);

            route_double_input_node(position, layout, place_info.node2pos, fc);
        }

        place_info.current_node++;
        layout.obstruct_coordinate({position.x, position.y, 0});
        layout.obstruct_coordinate({position.x, position.y, 1});

        // check if solution was found and update max_placed_nodes
        const auto found_solution = (place_info.current_node == ssg.nodes_to_place.size());
        if (place_info.current_node > max_placed_nodes)
        {
            max_placed_nodes = place_info.current_node;
        }

        return found_solution;
    }
    /**
     * Outputs placement information, including the current runtime, the number of evaluated paths in the search space
     * graphs and the layout dimensions.
     *
     * @param lyt Current layout.
     */
    void print_placement_info(const ObstrLyt& lyt) const
    {
        std::cout << "\n[i] Found improved solution:\n";

        // calculate the duration between start and end
        const auto end      = std::chrono::high_resolution_clock::now();
        const auto duration = end - start;

        // extract the duration components
        const auto us  = std::chrono::duration_cast<std::chrono::microseconds>(duration).count() % 1000;
        const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() % 1000;
        const auto sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();

        // output the elapsed time
        std::cout << fmt::format("[i]   Time taken:       {} s {} ms {} µs\n", sec, ms, us);
        std::cout << fmt::format("[i]   Layout dimension: {} × {} = {}\n", lyt.x() + 1, lyt.y() + 1, lyt.area());
        std::cout << fmt::format("[i]   #Wires: {}\n", lyt.num_wires() - lyt.num_pis() - lyt.num_pos());
        std::cout << fmt::format("[i]   #Crossings: {}\n", lyt.num_crossings());
        std::cout << fmt::format("[i]   ACP: {}\n", lyt.area() * (lyt.num_crossings() + 1));
    }
    /**
     * Initializes the layout with minimum width
     *
     * @param min_layout_width The minimum width of the layout.
     * @return The initialized layout.
     */
    ObstrLyt initialize_layout(uint64_t min_layout_width)
    {
        const auto layout_depth = ps.planar ? 0 : 1;
        Lyt        lyt{{min_layout_width - 1, 0, layout_depth}, row_clocking<Lyt>()};
        return obstruction_layout<Lyt>(lyt);
    }
    /**
     * Adjusts the layout size based on the last position.
     *
     * @param position The last position in the layout.
     * @param layout The layout to be adjusted.
     * @param ssg The search space graph.
     * @param place_info The placement information.
     */
    void adjust_layout_size(const tile<ObstrLyt>& position, ObstrLyt& layout, const search_space_graph<ObstrLyt>& ssg,
                            const placement_info<ObstrLyt>& place_info)
    {
        if (position.x == layout.x() && !ssg.network.is_po(ssg.nodes_to_place[place_info.current_node - 1]))
        {
            layout.resize({layout.x() + 1, layout.y(), layout.z()});
        }
        if (position.y == layout.y() && !ssg.network.is_po(ssg.nodes_to_place[place_info.current_node - 1]))
        {
            layout.resize({layout.x(), layout.y() + 1, layout.z()});
        }
    }
    std::uint64_t calculate_cost(const Lyt& layout, graph_oriented_layout_design_params::cost_objective cost_function)
    {
        uint64_t cost = 0;
        if (cost_function == graph_oriented_layout_design_params::cost_objective::AREA)
        {
            const auto bb = bounding_box_2d(layout);
            cost          = static_cast<uint64_t>(bb.get_max().x + 1u) * static_cast<uint64_t>(bb.get_max().y + 1u);
        }
        else if (cost_function == graph_oriented_layout_design_params::cost_objective::WIRES)
        {
            cost = layout.num_wires() - layout.num_pis() - layout.num_pos();
        }
        else if (cost_function == graph_oriented_layout_design_params::cost_objective::CROSSINGS)
        {
            cost = layout.num_crossings();
        }
        else if (cost_function == graph_oriented_layout_design_params::cost_objective::ACP)
        {
            const auto bb = bounding_box_2d(layout);
            cost          = (layout.num_crossings() + 1) *
                   (static_cast<uint64_t>(bb.get_max().x + 1u) * static_cast<uint64_t>(bb.get_max().y + 1u));
        }
        else if (cost_function == graph_oriented_layout_design_params::cost_objective::CUSTOM)
        {
            cost = custom_cost_objective(layout);
        }
        return cost;
    }
    /**
     * Computes a soft penalty for violating preferred PI order for a candidate PI placement.
     *
     * The penalty grows with left/right order violations against already placed PIs according to the configured
     * preferred order.
     *
     * @param ssg Current search-space graph.
     * @param candidate Candidate position for the next node.
     * @return Normalized penalty contribution to the expansion priority.
     */
    [[nodiscard]] double calculate_preferred_pi_order_penalty(const search_space_graph<ObstrLyt>& ssg,
                                                              const tile<ObstrLyt>&               candidate) const
    {
        if (!ps.prefer_input_pin_order)
        {
            return 0.0;
        }

        const auto current_node_idx = static_cast<uint64_t>(ssg.current_vertex.size());
        if (current_node_idx >= ssg.nodes_to_place.size())
        {
            return 0.0;
        }

        const auto current_node = ssg.nodes_to_place[current_node_idx];
        if (!ssg.network.is_pi(current_node))
        {
            return 0.0;
        }

        std::unordered_map<mockturtle::node<tec_nt>, uint64_t> pi_ranks{};
        pi_ranks.reserve(ssg.network.num_pis());

        uint64_t declaration_index = 0u;
        ssg.network.foreach_pi(
            [this, &pi_ranks, &declaration_index](const auto& pi)
            {
                const uint64_t rank = declaration_index < input_pin_order_ranks.size() ?
                                          input_pin_order_ranks[declaration_index] :
                                          declaration_index;
                pi_ranks[pi]        = rank;
                ++declaration_index;
            });

        const auto current_rank_it = pi_ranks.find(current_node);
        if (current_rank_it == pi_ranks.cend())
        {
            return 0.0;
        }

        const auto current_rank = current_rank_it->second;

        double penalty = 0.0;

        for (uint64_t node_idx = 0u; node_idx < current_node_idx; ++node_idx)
        {
            const auto prev_node = ssg.nodes_to_place[node_idx];
            if (!ssg.network.is_pi(prev_node))
            {
                continue;
            }

            const auto prev_rank_it = pi_ranks.find(prev_node);
            if (prev_rank_it == pi_ranks.cend())
            {
                continue;
            }

            const auto prev_rank = prev_rank_it->second;
            const auto prev_x    = ssg.current_vertex[node_idx].x;

            if (prev_rank < current_rank && prev_x >= candidate.x)
            {
                penalty += static_cast<double>(prev_x - candidate.x + 1u);
            }
            else if (prev_rank > current_rank && prev_x <= candidate.x)
            {
                penalty += static_cast<double>(candidate.x - prev_x + 1u);
            }
        }

        const auto normalizer = static_cast<double>(std::max<uint64_t>(1u, ssg.network.num_pis()));
        return penalty / normalizer;
    }
    /**
     * Computes a soft penalty for violating preferred PO order for a candidate PO placement.
     *
     * The penalty grows with left/right order violations against already placed POs according to the configured
     * preferred order.
     *
     * @param ssg Current search-space graph.
     * @param candidate Candidate position for the next node.
     * @return Normalized penalty contribution to the expansion priority.
     */
    [[nodiscard]] double calculate_preferred_po_order_penalty(const search_space_graph<ObstrLyt>& ssg,
                                                              const tile<ObstrLyt>&               candidate,
                                                              const ObstrLyt&                     layout) const
    {
        if (!ps.prefer_output_pin_order)
        {
            return 0.0;
        }

        const auto current_node_idx = static_cast<uint64_t>(ssg.current_vertex.size());
        if (current_node_idx >= ssg.nodes_to_place.size())
        {
            return 0.0;
        }

        const auto current_node = ssg.nodes_to_place[current_node_idx];
        if (!ssg.network.is_po(current_node))
        {
            return 0.0;
        }

        std::unordered_map<mockturtle::node<tec_nt>, uint64_t> po_ranks{};
        po_ranks.reserve(ssg.network.num_pos());

        uint64_t declaration_index = 0u;
        ssg.network.foreach_po(
            [this, &po_ranks, &declaration_index, &ssg](const auto& po, const auto po_index)
            {
                const auto po_node = ssg.network.get_node(po);
                uint64_t   rank    = declaration_index < output_pin_order_ranks.size() ?
                                         output_pin_order_ranks[declaration_index] :
                                         declaration_index;

                if (!output_pin_order_ranks_by_name.empty() && ssg.network.has_output_name(po_index))
                {
                    const auto po_name = ssg.network.get_output_name(po_index);
                    if (const auto it = output_pin_order_ranks_by_name.find(po_name);
                        it != output_pin_order_ranks_by_name.cend())
                    {
                        rank = it->second;
                    }
                }

                po_ranks[po_node] = rank;
                ++declaration_index;
            });

        const auto current_rank_it = po_ranks.find(current_node);
        if (current_rank_it == po_ranks.cend())
        {
            return 0.0;
        }

        const auto current_rank = current_rank_it->second;
        const auto num_pos      = std::max<uint64_t>(1u, ssg.network.num_pos());

        double penalty = 0.0;
        if (num_pos > 1u)
        {
            const auto target_x = (current_rank * layout.x()) / (num_pos - 1u);
            penalty += static_cast<double>(candidate.x > target_x ? candidate.x - target_x : target_x - candidate.x);
        }

        for (uint64_t node_idx = 0u; node_idx < current_node_idx; ++node_idx)
        {
            const auto prev_node = ssg.nodes_to_place[node_idx];
            if (!ssg.network.is_po(prev_node))
            {
                continue;
            }

            const auto prev_rank_it = po_ranks.find(prev_node);
            if (prev_rank_it == po_ranks.cend())
            {
                continue;
            }

            const auto prev_rank = prev_rank_it->second;
            const auto prev_x    = ssg.current_vertex[node_idx].x;

            if (prev_rank < current_rank && prev_x >= candidate.x)
            {
                penalty += static_cast<double>(prev_x - candidate.x + 1u);
            }
            else if (prev_rank > current_rank && prev_x <= candidate.x)
            {
                penalty += static_cast<double>(candidate.x - prev_x + 1u);
            }
        }

        const auto normalizer = static_cast<double>(num_pos);
        return penalty / normalizer;
    }
    /**
     * Generates the next possible positions with their priorities based on the layout and search space graph.
     *
     * @param possible_positions A vector of possible positions to be considered.
     * @param layout The layout to be used.
     * @param ssg The search space graph.
     * @return A pair containing the next positions with their priorities and an optional layout.
     */
    std::pair<std::vector<std::pair<coord_vec_type<ObstrLyt>, double>>, std::optional<ObstrLyt>>
    generate_next_positions(const coord_vec_type<ObstrLyt>& possible_positions, ObstrLyt& layout,
                            const search_space_graph<ObstrLyt>& ssg)
    {
        std::vector<std::pair<coord_vec_type<ObstrLyt>, double>> next_positions;
        next_positions.reserve(2 * ps.num_vertex_expansions);

        static constexpr double preferred_pi_order_penalty_weight = 0.5;
        static constexpr double preferred_po_order_penalty_weight = 0.5;

        for (const auto& position : possible_positions)
        {
            auto new_sequence = ssg.current_vertex;
            new_sequence.push_back(position);

            const auto remaining_nodes_to_place =
                static_cast<double>(ssg.nodes_to_place.size() - (ssg.current_vertex.size() + 1));

            if (ssg.cost == graph_oriented_layout_design_params::cost_objective::AREA)
            {
                // current layout size
                const double layout_size = static_cast<double>(((std::max(layout.x() - 1, position.x) + 1) *
                                                                (std::max(layout.y() - 1, position.y) + 1))) /
                                           static_cast<double>((ssg.nodes_to_place.size() * ssg.nodes_to_place.size()));

                // position of last placed node
                const double last_position =
                    static_cast<double>(((position.x + 1) * (position.y + 1))) /
                    static_cast<double>((ssg.nodes_to_place.size() * ssg.nodes_to_place.size()));

                double priority = remaining_nodes_to_place + layout_size + last_position;
                priority += preferred_pi_order_penalty_weight * calculate_preferred_pi_order_penalty(ssg, position);
                priority +=
                    preferred_po_order_penalty_weight * calculate_preferred_po_order_penalty(ssg, position, layout);
                next_positions.push_back({new_sequence, priority});
            }
            else
            {
                const double cost = static_cast<double>(calculate_cost(layout, ssg.cost)) /
                                    static_cast<double>(1000 * ssg.nodes_to_place.size());

                double priority = remaining_nodes_to_place + cost;
                priority += preferred_pi_order_penalty_weight * calculate_preferred_pi_order_penalty(ssg, position);
                priority +=
                    preferred_po_order_penalty_weight * calculate_preferred_po_order_penalty(ssg, position, layout);

                next_positions.push_back({new_sequence, priority});
            }
        }

        return {next_positions, std::nullopt};
    }
    /**
     * Computes possible expansions and their priorities for the current vertex in the search space graph.
     * It handles placement of nodes, checks for valid paths, and finds potential next positions based on priorities.
     *
     * @param ssg The search space graph.
     * @return A pair containing a vector of next positions with their priorities and an optional layout.
     * If an improved solution is found, the layout is returned.
     * If the layout is invalid or no improvement is possible, std::nullopt is returned.
     */
    [[nodiscard]] std::pair<std::vector<std::pair<coord_vec_type<ObstrLyt>, double>>, std::optional<ObstrLyt>>
    expand(search_space_graph<ObstrLyt>& ssg) noexcept
    {
        const auto min_layout_width = ssg.network.num_pis();

        std::vector<std::pair<coord_vec_type<ObstrLyt>, double>> next_positions;
        next_positions.reserve(2 * ps.num_vertex_expansions);

        auto layout = initialize_layout(min_layout_width);

        auto                             pi2node = reserve_input_nodes(layout, ssg.network);
        node_dict_type<ObstrLyt, tec_nt> node2pos{ssg.network};
        placement_info<ObstrLyt>         place_info{0ul, 0ul, node2pos, pi2node};

        coord_vec_type<ObstrLyt> possible_positions{};
        possible_positions.reserve(2 * ps.num_vertex_expansions);

        if (ssg.current_vertex.empty())
        {
            possible_positions = get_possible_positions(layout, ssg, place_info);
        }

        for (uint64_t idx = 0ul; idx < ssg.current_vertex.size(); ++idx)
        {
            const auto position = ssg.current_vertex[idx];

            bool found_solution = place_and_route(position, layout, ssg, place_info);

            uint64_t cost         = 0ul;
            uint64_t desired_cost = 0ul;

            bool     improve_solution = improve_custom_solution;
            uint64_t best_solution    = best_custom_solution;

            switch (ssg.cost)
            {
                case graph_oriented_layout_design_params::cost_objective::AREA:
                {
                    improve_solution = improve_area_solution;
                    best_solution    = best_area_solution;
                    break;
                }
                case graph_oriented_layout_design_params::cost_objective::WIRES:
                {
                    improve_solution = improve_wire_solution;
                    best_solution    = best_wire_solution;
                    break;
                }
                case graph_oriented_layout_design_params::cost_objective::CROSSINGS:
                {
                    improve_solution = improve_crossing_solution;
                    best_solution    = best_crossing_solution;
                    break;
                }
                case graph_oriented_layout_design_params::cost_objective::ACP:
                {
                    improve_solution = improve_acp_solution;
                    best_solution    = best_acp_solution;
                    break;
                }
                default:
                {
                    break;
                }
            }

            if (improve_solution)
            {
                cost = calculate_cost(layout, ssg.cost);
            }

            if (found_solution && (!improve_solution || cost <= best_solution))
            {
                cost = calculate_cost(layout, ssg.cost);

                switch (ssg.cost)
                {
                    case graph_oriented_layout_design_params::cost_objective::AREA:
                    {
                        improve_area_solution = true;
                        best_area_solution    = cost;
                        break;
                    }
                    case graph_oriented_layout_design_params::cost_objective::WIRES:
                    {
                        improve_wire_solution = true;
                        best_wire_solution    = cost;
                        break;
                    }
                    case graph_oriented_layout_design_params::cost_objective::CROSSINGS:
                    {
                        improve_crossing_solution = true;
                        best_crossing_solution    = cost;
                        break;
                    }
                    case graph_oriented_layout_design_params::cost_objective::ACP:
                    {
                        improve_acp_solution = true;
                        best_acp_solution    = cost;
                        break;
                    }
                    default:
                    {
                        improve_custom_solution = true;
                        best_custom_solution    = cost;
                        break;
                    }
                }

                auto apply_plo = true;
                if (ps.straight_inverters)
                {
                    layout.foreach_po(
                        [&layout, &apply_plo](const auto& gate)
                        {
                            if (const auto coord = layout.get_tile(layout.get_node(gate));
                                layout.is_inv(layout.get_node(layout.incoming_data_flow(coord).front())))
                            {
                                apply_plo = false;
                            }
                        });
                }

                if (apply_plo)
                {
                    if constexpr (is_cartesian_layout_v<ObstrLyt>)
                    {
                        fiction::post_layout_optimization_params plo_params{};
                        plo_params.optimize_pos_only   = true;
                        plo_params.planar_optimization = ps.planar;

                        fiction::post_layout_optimization(layout, plo_params);
                    }
                }

                const auto bb_after_plo = fiction::bounding_box_2d(layout);
                layout.resize({bb_after_plo.get_max().x, bb_after_plo.get_max().y, layout.z()});

                desired_cost = calculate_cost(layout, ps.cost);

                if (desired_cost < best_optimized_solution)
                {
                    best_optimized_solution = desired_cost;

                    if (ps.verbose)
                    {
                        print_placement_info(layout);
                    }

                    return {{}, layout};
                }

                return {{}, std::nullopt};
            }

            improve_solution = improve_custom_solution;
            best_solution    = best_custom_solution;

            switch (ssg.cost)
            {
                case graph_oriented_layout_design_params::cost_objective::AREA:
                {
                    improve_solution = improve_area_solution;
                    best_solution    = best_area_solution;
                    break;
                }
                case graph_oriented_layout_design_params::cost_objective::WIRES:
                {
                    improve_solution = improve_wire_solution;
                    best_solution    = best_wire_solution;
                    break;
                }
                case graph_oriented_layout_design_params::cost_objective::CROSSINGS:
                {
                    improve_solution = improve_crossing_solution;
                    best_solution    = best_crossing_solution;
                    break;
                }
                case graph_oriented_layout_design_params::cost_objective::ACP:
                {
                    improve_solution = improve_acp_solution;
                    best_solution    = best_acp_solution;
                    break;
                }
                default:
                {
                    break;
                }
            }
            if (improve_solution && cost >= best_solution)
            {
                return {{}, std::nullopt};
            }

            adjust_layout_size(position, layout, ssg, place_info);
            // check if it's the last position in the current vertex
            if (idx == (ssg.current_vertex.size() - 1))
            {
                if (!valid_layout(layout, ssg, place_info))
                {
                    return {{}, std::nullopt};
                }

                possible_positions = get_possible_positions(layout, ssg, place_info);
            }
        }

        return generate_next_positions(possible_positions, layout, ssg);
    }
    /**
     * This function performs an expansion step on the given SSG and updates the frontier and cost information.
     *
     * @param ssg The search space graph to process.
     * @return An optional layout. Returns a layout if one is found during expansion; otherwise, std::nullopt.
     */
    std::optional<Lyt> process_ssg(search_space_graph<ObstrLyt>& ssg)
    {
        if (ssg.frontier_flag)
        {
            const auto expansion = expand(ssg);
            if (expansion.second)
            {
                return expansion.second;
            }

            // Update costs and frontier
            for (const auto& [next, cost] : expansion.first)
            {
                if (ssg.cost_so_far.find(next) == ssg.cost_so_far.cend() || cost < ssg.cost_so_far[next])
                {
                    ssg.cost_so_far[next] = cost;
                    double priority       = cost;
                    ssg.frontier.put(next, priority);
                }
            }
        }
        return std::nullopt;
    }
    /**
     * Initializes the allowed positions for primary inputs (PIs), the cost for each search space graph and the maximum
     * number of expansions.
     */
    void initialize_pis_cost_and_num_expansions() noexcept
    {
        static constexpr std::array pattern{pi_locations::TOP, pi_locations::LEFT, pi_locations::TOP_AND_LEFT};

        std::size_t idx = 0;  // index into the pattern

        for (auto& graph : ssg_vec)
        {
            if constexpr (is_hexagonal_layout_v<ObstrLyt>)
            {
                graph.pi_locs = pi_locations::TOP;
            }
            else
            {
                graph.pi_locs = pattern.at(idx % pattern.size());
            }
            ++idx;  // move to next pattern element

            graph.cost_so_far[graph.current_vertex] = 0;
        }
    }
    /**
     * Initializes the networks and nodes to place.
     */
    void initialize_networks_and_nodes_to_place() noexcept
    {
        // helper function to prepare nodes to place
        const auto prepare_nodes_to_place = [](auto& network, auto& nodes_to_place) noexcept
        {
            nodes_to_place.reserve(network.size());
            network.foreach_node(
                [&nodes_to_place, &network](const auto& n)
                {
                    if (!network.is_constant(n) && !network.is_po(n))
                    {
                        nodes_to_place.push_back(n);
                    }
                });

            network.foreach_co([&nodes_to_place, &network](const auto& f)
                               { nodes_to_place.push_back(network.get_node(f)); });
        };
        // helper function to prioritize preferred network PI order at the beginning of the placement list
        const auto reorder_pi_nodes = [&](const auto& network, auto& nodes_to_place) noexcept
        {
            if (!ps.prefer_input_pin_order)
            {
                return;
            }

            using node_t = typename std::decay_t<decltype(nodes_to_place)>::value_type;

            std::unordered_map<node_t, uint64_t> pi_ranks{};
            pi_ranks.reserve(network.num_pis());

            uint64_t declaration_index = 0u;
            network.foreach_pi(
                [&pi_ranks, &declaration_index, this](const auto& pi)
                {
                    assert(declaration_index < input_pin_order_ranks.size());
                    pi_ranks[pi] = input_pin_order_ranks[declaration_index++];
                });

            std::stable_sort(nodes_to_place.begin(), nodes_to_place.end(),
                             [&pi_ranks](const auto& lhs, const auto& rhs) noexcept
                             {
                                 const auto lhs_it    = pi_ranks.find(lhs);
                                 const auto rhs_it    = pi_ranks.find(rhs);
                                 const bool lhs_is_pi = lhs_it != pi_ranks.cend();
                                 const bool rhs_is_pi = rhs_it != pi_ranks.cend();

                                 if (lhs_is_pi != rhs_is_pi)
                                 {
                                     return lhs_is_pi;
                                 }
                                 if (lhs_is_pi && rhs_is_pi)
                                 {
                                     return lhs_it->second < rhs_it->second;
                                 }

                                 return false;
                             });
        };
        // set cost objectives based on effort mode and cost objective
        const auto set_costs = [&](const uint64_t start_idx, const uint64_t end_idx, const auto cost_objective)
        {
            for (uint64_t i = start_idx; i < end_idx; ++i)
            {
                ssg_vec[i].cost = cost_objective;
            }
        };

        // In native hex mode, PIs are placed on the top border only.
        const uint64_t num_possible_pi_locations = 1u;

        // helper function to assign networks and nodes
        const auto assign_networks_and_nodes = [&](uint64_t base_index, auto& breadth_co_to_ci, auto& breadth_ci_to_co,
                                                   auto& depth_co_to_ci, auto& depth_ci_to_co, auto& breadth_co_nodes,
                                                   auto& breadth_ci_nodes, auto& depth_co_nodes, auto& depth_ci_nodes)
        {
            for (uint64_t i = 0; i < num_possible_pi_locations; ++i)
            {
                ssg_vec[base_index + (0 * num_possible_pi_locations) + i].network = breadth_co_to_ci;
                ssg_vec[base_index + (1 * num_possible_pi_locations) + i].network = breadth_ci_to_co;
                ssg_vec[base_index + (2 * num_possible_pi_locations) + i].network = depth_co_to_ci;
                ssg_vec[base_index + (3 * num_possible_pi_locations) + i].network = depth_ci_to_co;

                ssg_vec[base_index + (0 * num_possible_pi_locations) + i].nodes_to_place = breadth_co_nodes;
                ssg_vec[base_index + (1 * num_possible_pi_locations) + i].nodes_to_place = breadth_ci_nodes;
                ssg_vec[base_index + (2 * num_possible_pi_locations) + i].nodes_to_place = depth_co_nodes;
                ssg_vec[base_index + (3 * num_possible_pi_locations) + i].nodes_to_place = depth_ci_nodes;
            }
        };

        fanout_substitution_params params{};
        params.strategy = fanout_substitution_params::substitution_strategy::BREADTH;
        mockturtle::fanout_view network_substituted_breadth{fanout_substitution<tec_nt>(ntk, params)};

        topo_view_co_to_ci<decltype(network_substituted_breadth)> network_breadth_co_to_ci{network_substituted_breadth,
                                                                                           seed};

        // prepare initial nodes to place
        std::vector<mockturtle::node<decltype(network_breadth_co_to_ci)>> nodes_to_place_breadth_co_to_ci{};
        prepare_nodes_to_place(network_breadth_co_to_ci, nodes_to_place_breadth_co_to_ci);
        reorder_pi_nodes(network_breadth_co_to_ci, nodes_to_place_breadth_co_to_ci);

        // set initial cost
        for (uint64_t i = 0; i < num_search_space_graphs_high_efficiency; ++i)
        {
            ssg_vec[i].network        = network_breadth_co_to_ci;
            ssg_vec[i].nodes_to_place = nodes_to_place_breadth_co_to_ci;
            ssg_vec[i].cost           = ps.cost;
        }

        // further network and nodes initialization for high-, highest-, and maximum-effort
        if (ps.mode != graph_oriented_layout_design_params::effort_mode::HIGH_EFFICIENCY)
        {
            params.strategy = fanout_substitution_params::substitution_strategy::DEPTH;
            mockturtle::fanout_view network_substituted_depth{fanout_substitution<tec_nt>(ntk, params)};

            topo_view_ci_to_co<decltype(network_substituted_breadth)> network_breadth_ci_to_co{
                network_substituted_breadth, seed};
            topo_view_co_to_ci<decltype(network_substituted_depth)> network_depth_co_to_ci{network_substituted_depth,
                                                                                           seed};
            topo_view_ci_to_co<decltype(network_substituted_depth)> network_depth_ci_to_co{network_substituted_depth,
                                                                                           seed};

            // prepare nodes to place for additional networks
            std::vector<mockturtle::node<decltype(network_breadth_ci_to_co)>> nodes_to_place_breadth_ci_to_co{};
            std::vector<mockturtle::node<decltype(network_depth_co_to_ci)>>   nodes_to_place_depth_co_to_ci{};
            std::vector<mockturtle::node<decltype(network_depth_ci_to_co)>>   nodes_to_place_depth_ci_to_co{};

            prepare_nodes_to_place(network_breadth_ci_to_co, nodes_to_place_breadth_ci_to_co);
            prepare_nodes_to_place(network_depth_co_to_ci, nodes_to_place_depth_co_to_ci);
            prepare_nodes_to_place(network_depth_ci_to_co, nodes_to_place_depth_ci_to_co);
            reorder_pi_nodes(network_breadth_ci_to_co, nodes_to_place_breadth_ci_to_co);
            reorder_pi_nodes(network_depth_co_to_ci, nodes_to_place_depth_co_to_ci);
            reorder_pi_nodes(network_depth_ci_to_co, nodes_to_place_depth_ci_to_co);

            // prepare 4 search space graphs
            assign_networks_and_nodes(0, network_breadth_co_to_ci, network_breadth_ci_to_co, network_depth_co_to_ci,
                                      network_depth_ci_to_co, nodes_to_place_breadth_co_to_ci,
                                      nodes_to_place_breadth_ci_to_co, nodes_to_place_depth_co_to_ci,
                                      nodes_to_place_depth_ci_to_co);

            if (ps.mode != graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT)
            {
                for (uint64_t j = num_search_space_graphs_high_effort;
                     j <= (num_search_space_graphs_highest_effort - num_search_space_graphs_high_effort);
                     j += num_search_space_graphs_high_effort)
                {
                    assign_networks_and_nodes(j, network_breadth_co_to_ci, network_breadth_ci_to_co,
                                              network_depth_co_to_ci, network_depth_ci_to_co,
                                              nodes_to_place_breadth_co_to_ci, nodes_to_place_breadth_ci_to_co,
                                              nodes_to_place_depth_co_to_ci, nodes_to_place_depth_ci_to_co);
                }

                if (ps.cost == graph_oriented_layout_design_params::cost_objective::CUSTOM)
                {
                    assign_networks_and_nodes(num_search_space_graphs_highest_effort, network_breadth_co_to_ci,
                                              network_breadth_ci_to_co, network_depth_co_to_ci, network_depth_ci_to_co,
                                              nodes_to_place_breadth_co_to_ci, nodes_to_place_breadth_ci_to_co,
                                              nodes_to_place_depth_co_to_ci, nodes_to_place_depth_ci_to_co);
                }

                if (ps.mode != graph_oriented_layout_design_params::effort_mode::HIGHEST_EFFORT)
                {
                    params.strategy = fanout_substitution_params::substitution_strategy::RANDOM;
                    params.seed     = seed;
                    mockturtle::fanout_view network_substituted_random{fanout_substitution<tec_nt>(ntk, params)};

                    topo_view_co_to_ci_random<decltype(network_substituted_random)> network_breadth_co_to_ci_random{
                        network_substituted_random, seed};
                    topo_view_ci_to_co_random<decltype(network_substituted_random)> network_breadth_ci_to_co_random{
                        network_substituted_random, seed};
                    topo_view_co_to_ci_random<decltype(network_substituted_random)> network_depth_co_to_ci_random{
                        network_substituted_random, seed};
                    topo_view_ci_to_co_random<decltype(network_substituted_random)> network_depth_ci_to_co_random{
                        network_substituted_random, seed};

                    // prepare nodes to place for additional networks
                    std::vector<mockturtle::node<decltype(network_breadth_co_to_ci_random)>>
                        nodes_to_place_breadth_co_to_ci_random{};
                    std::vector<mockturtle::node<decltype(network_breadth_ci_to_co_random)>>
                        nodes_to_place_breadth_ci_to_co_random{};
                    std::vector<mockturtle::node<decltype(network_depth_co_to_ci_random)>>
                        nodes_to_place_depth_co_to_ci_random{};
                    std::vector<mockturtle::node<decltype(network_depth_ci_to_co_random)>>
                        nodes_to_place_depth_ci_to_co_random{};

                    prepare_nodes_to_place(network_breadth_co_to_ci_random, nodes_to_place_breadth_co_to_ci_random);
                    prepare_nodes_to_place(network_breadth_ci_to_co_random, nodes_to_place_breadth_ci_to_co_random);
                    prepare_nodes_to_place(network_depth_co_to_ci_random, nodes_to_place_depth_co_to_ci_random);
                    prepare_nodes_to_place(network_depth_ci_to_co_random, nodes_to_place_depth_ci_to_co_random);
                    reorder_pi_nodes(network_breadth_co_to_ci_random, nodes_to_place_breadth_co_to_ci_random);
                    reorder_pi_nodes(network_breadth_ci_to_co_random, nodes_to_place_breadth_ci_to_co_random);
                    reorder_pi_nodes(network_depth_co_to_ci_random, nodes_to_place_depth_co_to_ci_random);
                    reorder_pi_nodes(network_depth_ci_to_co_random, nodes_to_place_depth_ci_to_co_random);

                    if (ps.cost != graph_oriented_layout_design_params::cost_objective::CUSTOM)
                    {
                        for (uint64_t j = num_search_space_graphs_highest_effort;
                             j <= (num_search_space_graphs_maximum_effort - num_search_space_graphs_high_effort);
                             j += num_search_space_graphs_high_effort)
                        {
                            assign_networks_and_nodes(
                                j, network_breadth_co_to_ci_random, network_breadth_ci_to_co_random,
                                network_depth_co_to_ci_random, network_depth_ci_to_co_random,
                                nodes_to_place_breadth_co_to_ci_random, nodes_to_place_breadth_ci_to_co_random,
                                nodes_to_place_depth_co_to_ci_random, nodes_to_place_depth_ci_to_co_random);
                        }
                    }
                    else
                    {
                        for (uint64_t j = num_search_space_graphs_highest_effort_custom;
                             j <= (num_search_space_graphs_maximum_effort_custom - num_search_space_graphs_high_effort);
                             j += num_search_space_graphs_high_effort)
                        {
                            assign_networks_and_nodes(
                                j, network_breadth_co_to_ci_random, network_breadth_ci_to_co_random,
                                network_depth_co_to_ci_random, network_depth_ci_to_co_random,
                                nodes_to_place_breadth_co_to_ci_random, nodes_to_place_breadth_ci_to_co_random,
                                nodes_to_place_depth_co_to_ci_random, nodes_to_place_depth_ci_to_co_random);
                        }
                    }
                }
            }
            const std::array core_objectives = {graph_oriented_layout_design_params::cost_objective::AREA,
                                                graph_oriented_layout_design_params::cost_objective::WIRES,
                                                graph_oriented_layout_design_params::cost_objective::CROSSINGS,
                                                graph_oriented_layout_design_params::cost_objective::ACP};

            // batch of 4 SSGs (all combinations of 1 PI location, 2 fanout substitution strategies, and 2
            // topological orderings)
            const auto ssg_batch = num_search_space_graphs_high_effort;

            if (ps.mode == graph_oriented_layout_design_params::effort_mode::HIGH_EFFORT)
            {
                set_costs(0, ssg_batch, ps.cost);
            }
            else
            {
                std::uint64_t offset = 0;

                for (auto obj : core_objectives)
                {
                    set_costs(offset, offset + ssg_batch, obj);
                    offset += ssg_batch;
                }

                if (ps.cost == graph_oriented_layout_design_params::cost_objective::CUSTOM)
                {
                    set_costs(offset, num_search_space_graphs_highest_effort_custom,
                              graph_oriented_layout_design_params::cost_objective::CUSTOM);
                }
            }
            if (ps.mode == graph_oriented_layout_design_params::effort_mode::MAXIMUM_EFFORT)
            {
                std::uint64_t offset = (ps.cost == graph_oriented_layout_design_params::cost_objective::CUSTOM) ?
                                           num_search_space_graphs_highest_effort_custom :
                                           num_search_space_graphs_highest_effort;

                for (auto obj : core_objectives)
                {
                    set_costs(offset, offset + ssg_batch, obj);
                    offset += ssg_batch;
                }

                if (ps.cost == graph_oriented_layout_design_params::cost_objective::CUSTOM)
                {
                    set_costs(offset, num_search_space_graphs_maximum_effort_custom,
                              graph_oriented_layout_design_params::cost_objective::CUSTOM);
                }
            }
        }
    }
    /**
     * Initialize the search space graphs.
     */
    void initialize() noexcept
    {
        // initial setup for networks and nodes to place
        initialize_networks_and_nodes_to_place();

        // initial setup for PIs and cost
        initialize_pis_cost_and_num_expansions();
    }
};

}  // namespace detail

/**
 * A scalable and efficient placement & routing approach for hexagonal layouts based on spanning a search space graph
 * of partial layouts and finding a path to one of its leaves, i.e., a complete layout.
 *
 * This variant is intended for native hexagonal GOLD and uses row-based clocking assumptions.
 *
 * @tparam Lyt Hexagonal gate-level layout type.
 * @tparam Ntk Network type.
 * @param ntk The network to be placed and routed.
 * @param ps The parameters for the A* priority routing algorithm. Defaults to an empty parameter set.
 * @param pst A pointer to a statistics object to record execution details. Defaults to nullptr.
 * @param custom_cost_objective A custom cost objective that is evaluated at every expansion of the search space graph.
 * Should be a function that can be calculated based on the current partial layout and returns an uint64_t that should
 * be minimized.
 * @return The smallest layout yielded by the graph-oriented layout design algorithm under the given parameters.
 */
template <typename Lyt, typename Ntk>
std::optional<Lyt> graph_oriented_layout_design_hex(Ntk& ntk, graph_oriented_layout_design_params ps = {},
                                                    graph_oriented_layout_design_stats* pst                   = nullptr,
                                                    std::function<uint64_t(const Lyt&)> custom_cost_objective = nullptr)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(mockturtle::is_network_type_v<Ntk>,
                  "Ntk is not a network type");  // Ntk is being converted to a technology_network anyway, therefore,
                                                 // this is the only relevant check here

    if (has_high_degree_fanin_nodes(ntk, 2))
    {
        throw high_degree_fanin_exception();
    }

    if (ps.cost == graph_oriented_layout_design_params::cost_objective::CUSTOM && !custom_cost_objective)
    {
        throw std::invalid_argument("No custom cost objective provided.");
    }

    graph_oriented_layout_design_stats                      st{};
    detail::graph_oriented_layout_design_hex_impl<Lyt, Ntk> p{ntk, ps, st, custom_cost_objective};

    const auto result = p.run();

    if (pst)
    {
        *pst = st;
    }

    return result;
}

}  // namespace fiction

#endif  // FICTION_GRAPH_ORIENTED_LAYOUT_DESIGN_HEX_HPP
