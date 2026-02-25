//
// Created by marcel on 23.02.26.
//

#ifndef FICTION_WRITE_DOT_RETURN_PATH_OVERLAY_LAYOUT_HPP
#define FICTION_WRITE_DOT_RETURN_PATH_OVERLAY_LAYOUT_HPP

#include "fiction/algorithms/physical_design/route_return_path.hpp"
#include "fiction/io/dot_drawers.hpp"
#include "fiction/traits.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace fiction
{

/**
 * @brief Directed edge key for DOT overlay highlighting.
 */
struct return_path_overlay_edge
{
    /**
     * @brief Source tile key.
     */
    uint64_t source{};
    /**
     * @brief Target tile key.
     */
    uint64_t target{};

    /**
     * @brief Equality operator.
     *
     * @param other Other edge.
     * @return `true` iff both source and target match.
     */
    [[nodiscard]] bool operator==(const return_path_overlay_edge& other) const noexcept
    {
        return source == other.source && target == other.target;
    }
};

/**
 * @brief Hash function for @ref return_path_overlay_edge.
 */
struct return_path_overlay_edge_hash
{
    /**
     * @brief Hashes an edge key.
     *
     * @param edge Edge key.
     * @return Hash value.
     */
    [[nodiscard]] std::size_t operator()(const return_path_overlay_edge& edge) const noexcept
    {
        return std::hash<uint64_t>{}(edge.source) ^ (std::hash<uint64_t>{}(edge.target) << 1u);
    }
};

/**
 * @brief Overlay information describing which tiles and edges were added by return-path routing.
 */
struct return_path_overlay
{
    /**
     * @brief Added tile keys.
     */
    std::unordered_set<uint64_t> overlay_tiles{};
    /**
     * @brief Added edge keys.
     */
    std::unordered_set<return_path_overlay_edge, return_path_overlay_edge_hash> overlay_edges{};
};

/**
 * @brief Visualization parameters for return-path overlay DOT output.
 */
struct return_path_overlay_drawer_params
{
    /**
     * @brief If `true`, consider only clock-valid fanins when drawing edges.
     */
    bool respect_clocking = true;
    /**
     * @brief Fill color used for newly added return-path tiles.
     */
    std::string overlay_tile_fillcolor = "lightcoral";
    /**
     * @brief Edge color used for newly added return-path edges.
     */
    std::string overlay_edge_color = "firebrick";
    /**
     * @brief Pen width used for newly added return-path edges.
     */
    double overlay_edge_penwidth = 2.5;
};

namespace detail
{

/**
 * @brief Collects all occupied tile keys of a layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @return Occupied tile keys.
 */
template <typename Lyt>
[[nodiscard]] std::unordered_set<uint64_t> collect_occupied_tile_keys(const Lyt& lyt)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    std::unordered_set<uint64_t> occupied_tile_keys{};

    for (uint64_t z = 0u; z <= lyt.z(); ++z)
    {
        for (uint64_t y = 0u; y <= lyt.y(); ++y)
        {
            for (uint64_t x = 0u; x <= lyt.x(); ++x)
            {
                const tile<Lyt> t{x, y, z};
                if (!lyt.is_empty_tile(t))
                {
                    occupied_tile_keys.insert(static_cast<uint64_t>(t));
                }
            }
        }
    }

    return occupied_tile_keys;
}

/**
 * @brief Collects all fanin edges of a layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout.
 * @param respect_clocking If `true`, collect only clock-valid fanin edges.
 * @return Directed fanin edge keys.
 */
template <typename Lyt>
[[nodiscard]] std::unordered_set<return_path_overlay_edge, return_path_overlay_edge_hash>
collect_fanin_edge_keys(const Lyt& lyt, const bool respect_clocking)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    std::unordered_set<return_path_overlay_edge, return_path_overlay_edge_hash> edge_keys{};

    lyt.foreach_node(
        [&lyt, &edge_keys, respect_clocking](const auto& n)
        {
            const auto target_tile_key = static_cast<uint64_t>(lyt.get_tile(n));

            if (respect_clocking)
            {
                const auto collect_edge = [&edge_keys, target_tile_key](const auto& fanin_signal)
                { edge_keys.insert({static_cast<uint64_t>(static_cast<tile<Lyt>>(fanin_signal)), target_tile_key}); };

                lyt.foreach_fanin(n, std::move(collect_edge));
            }
            else
            {
                const auto collect_edge = [&edge_keys, target_tile_key](const auto& fanin_signal)
                { edge_keys.insert({static_cast<uint64_t>(static_cast<tile<Lyt>>(fanin_signal)), target_tile_key}); };

                lyt.template foreach_fanin<decltype(collect_edge), false>(n, std::move(collect_edge));
            }
        });

    return edge_keys;
}

/**
 * @brief Draws all edges of a layout into a DOT edge stream.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam Drawer Drawer type.
 * @param lyt Layout to draw.
 * @param drawer Drawer instance.
 * @param overlay Overlay edge information.
 * @param params Overlay visualization parameters.
 * @param edges Output edge stream.
 */
template <typename Lyt, typename Drawer>
void draw_dot_edges(const Lyt& lyt, const Drawer& drawer, const return_path_overlay& overlay,
                    const return_path_overlay_drawer_params& params, std::stringstream& edges)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    lyt.foreach_node(
        [&lyt, &drawer, &overlay, &params, &edges](const auto& n)
        {
            const auto target_tile     = lyt.get_tile(n);
            const auto target_tile_key = static_cast<uint64_t>(target_tile);

            const auto draw_edge =
                [&lyt, &drawer, &overlay, &params, &edges, target_tile_key, target_tile](const auto& fanin_signal)
            {
                const auto source_tile     = static_cast<tile<Lyt>>(fanin_signal);
                const auto source_tile_key = static_cast<uint64_t>(source_tile);
                const auto edge_style      = drawer.signal_style(lyt, fanin_signal);
                const bool is_overlay_edge = overlay.overlay_edges.count({source_tile_key, target_tile_key}) > 0u;

                if (is_overlay_edge)
                {
                    edges << fmt::format("{} -> {} [style={}, color={}, penwidth={}];\n", drawer.tile_id(source_tile),
                                         drawer.tile_id(target_tile), edge_style, params.overlay_edge_color,
                                         params.overlay_edge_penwidth);
                }
                else
                {
                    edges << fmt::format("{} -> {} [style={}];\n", drawer.tile_id(source_tile),
                                         drawer.tile_id(target_tile), edge_style);
                }
            };

            if (params.respect_clocking)
            {
                lyt.foreach_fanin(n, draw_edge);
            }
            else
            {
                lyt.template foreach_fanin<decltype(draw_edge), false>(n, std::move(draw_edge));
            }
        });
}

}  // namespace detail

/**
 * @brief Creates a non-routed reference layout in the same footprint used by return-path routing.
 *
 * The returned layout contains the shifted original circuit and corridor clock assignments, but no routed return paths.
 *
 * @tparam Lyt Gate-level layout type.
 * @param original_layout Original layout.
 * @param params Return-path routing parameters.
 * @return Non-routed reference layout matching the routed footprint.
 */
template <typename Lyt>
[[nodiscard]] Lyt create_return_path_reference_layout(const Lyt&                      original_layout,
                                                      const route_return_path_params& params = {})
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    const auto num_pairs = static_cast<uint32_t>(std::min(original_layout.num_pis(), original_layout.num_pos()));

    if (num_pairs == 0u)
    {
        return original_layout;
    }

    const uint32_t top_margin = detail::determine_required_top_margin(num_pairs, params);
    const uint32_t bottom_margin =
        detail::determine_required_lane_margin(num_pairs, params.lane_spacing, params.bottom_margin);
    const uint32_t right_margin =
        detail::determine_required_lane_margin(num_pairs, params.lane_spacing, params.right_margin);

    auto reference_layout = detail::create_extended_layout(original_layout, top_margin, bottom_margin, right_margin);

    detail::copy_layout_with_vertical_offset(original_layout, reference_layout, top_margin);
    detail::assign_corridor_clock_numbers(reference_layout, static_cast<uint32_t>(original_layout.x()),
                                          static_cast<uint32_t>(original_layout.y()), top_margin);

    return reference_layout;
}

/**
 * @brief Computes return-path overlay tiles and edges by comparing reference and routed layouts.
 *
 * @tparam Lyt Gate-level layout type.
 * @param reference_layout Non-routed reference layout in routed footprint.
 * @param routed_layout Routed layout.
 * @param respect_clocking If `true`, compare clock-valid edges only.
 * @return Overlay information.
 * @throws std::invalid_argument If layout dimensions differ.
 */
template <typename Lyt>
[[nodiscard]] return_path_overlay determine_return_path_overlay(const Lyt& reference_layout, const Lyt& routed_layout,
                                                                const bool respect_clocking = true)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    if (reference_layout.x() != routed_layout.x() || reference_layout.y() != routed_layout.y() ||
        reference_layout.z() != routed_layout.z())
    {
        throw std::invalid_argument{"Reference and routed layouts must have identical dimensions"};
    }

    return_path_overlay overlay{};

    const auto reference_tiles = detail::collect_occupied_tile_keys(reference_layout);
    const auto routed_tiles    = detail::collect_occupied_tile_keys(routed_layout);

    for (const auto tile_key : routed_tiles)
    {
        if (reference_tiles.count(tile_key) == 0u)
        {
            overlay.overlay_tiles.insert(tile_key);
        }
    }

    const auto reference_edges = detail::collect_fanin_edge_keys(reference_layout, respect_clocking);
    const auto routed_edges    = detail::collect_fanin_edge_keys(routed_layout, respect_clocking);

    for (const auto& edge_key : routed_edges)
    {
        if (reference_edges.count(edge_key) == 0u)
        {
            overlay.overlay_edges.insert(edge_key);
        }
    }

    return overlay;
}

/**
 * @brief Writes routed layout DOT output with highlighted return-path additions.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam ClockColors Draw clock colors if `true`.
 * @tparam DrawIndexes Draw node indexes if `true`.
 * @param reference_layout Non-routed reference layout in routed footprint.
 * @param routed_layout Routed layout.
 * @param os Output stream.
 * @param params Overlay visualization parameters.
 */
template <typename Lyt, bool ClockColors = false, bool DrawIndexes = false>
void write_dot_return_path_overlay_layout(const Lyt& reference_layout, const Lyt& routed_layout, std::ostream& os,
                                          const return_path_overlay_drawer_params& params = {})
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    const auto overlay = determine_return_path_overlay(reference_layout, routed_layout, params.respect_clocking);

    gate_layout_hexagonal_drawer<Lyt, ClockColors, DrawIndexes> drawer{};

    std::stringstream nodes{}, edges{}, topology{};

    auto node_attributes = drawer.additional_node_attributes();
    node_attributes.emplace_back("style=filled");

    nodes << fmt::format("node [{}];\n", fmt::join(node_attributes, ", "));

    routed_layout.foreach_ground_tile(
        [&routed_layout, &drawer, &overlay, &params, &nodes](const auto& t)
        {
            const auto tile_key  = static_cast<uint64_t>(t);
            const auto fillcolor = overlay.overlay_tiles.count(tile_key) > 0u ? params.overlay_tile_fillcolor :
                                                                                drawer.tile_fillcolor(routed_layout, t);

            nodes << fmt::format("{} [label=\"{}\", fillcolor={}];\n", drawer.tile_id(t),
                                 drawer.tile_label(routed_layout, t), fillcolor);
        });

    edges << "edge [constraint=false];\n";

    detail::draw_dot_edges(routed_layout, drawer, overlay, params, edges);

    topology << drawer.enforce_topology(routed_layout);

    os << fmt::format("digraph layout {{  // Generated by {} ({})\n{};\n\n", FICTION_VERSION, FICTION_REPO,
                      fmt::join(drawer.additional_graph_attributes(), ";\n"))
       << nodes.rdbuf() << '\n'
       << edges.rdbuf() << '\n'
       << topology.rdbuf() << "}\n";
}

/**
 * @brief Writes routed layout DOT output with highlighted return-path additions into a file.
 *
 * @tparam Lyt Gate-level layout type.
 * @tparam ClockColors Draw clock colors if `true`.
 * @tparam DrawIndexes Draw node indexes if `true`.
 * @param reference_layout Non-routed reference layout in routed footprint.
 * @param routed_layout Routed layout.
 * @param filename Output filename.
 * @param params Overlay visualization parameters.
 * @throws std::ofstream::failure If file cannot be opened.
 */
template <typename Lyt, bool ClockColors = false, bool DrawIndexes = false>
void write_dot_return_path_overlay_layout(const Lyt& reference_layout, const Lyt& routed_layout,
                                          const std::string_view&                  filename,
                                          const return_path_overlay_drawer_params& params = {})
{
    std::ofstream os{std::string{filename}, std::ofstream::out};

    if (!os.is_open())
    {
        throw std::ofstream::failure("could not open file");
    }

    write_dot_return_path_overlay_layout<Lyt, ClockColors, DrawIndexes>(reference_layout, routed_layout, os, params);
    os.close();
}

}  // namespace fiction

#endif  // FICTION_WRITE_DOT_RETURN_PATH_OVERLAY_LAYOUT_HPP
