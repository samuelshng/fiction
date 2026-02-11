//
// Created by marcel on 10.02.26.
//

#ifndef FICTION_UNSCRAMBLE_PINS_HPP
#define FICTION_UNSCRAMBLE_PINS_HPP

#include "fiction/layouts/clocking_scheme.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/network_utils.hpp"
#include "fiction/utils/placement_utils.hpp"

#include <mockturtle/traits.hpp>
#include <mockturtle/utils/stopwatch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <vector>

namespace fiction
{

/**
 * Parameters for the pin unscrambling algorithm.
 */
struct unscramble_pins_params
{};

/**
 * Statistics for the pin unscrambling algorithm.
 */
struct unscramble_pins_stats
{
    mockturtle::stopwatch<>::duration time_total{0};

    void report(std::ostream& out = std::cout) const
    {
        out << fmt::format("[i] total time = {:.2f} secs\n", mockturtle::to_seconds(time_total));
    }
};

namespace detail
{

/**
 * Extracts the layout coordinates for a given list of pin nodes.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @param pins Vector of pin nodes.
 * @return Vector of coordinates corresponding to the pins.
 */
template <typename Lyt>
std::vector<tile<Lyt>> determine_pin_coordinates(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& pins)
{
    std::vector<tile<Lyt>> coords{};
    coords.reserve(pins.size());
    for (const auto& pin : pins)
    {
        coords.push_back(lyt.get_tile(pin));
    }

    return coords;
}
/**
 * Calculates the number of rows required to route the pins from their current locations to the desired permutation
 * slots. Assumes pointy-top hexagonal layout constraints where horizontal movement requires vertical steps (2 rows per
 * 1 column shift).
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @param current_permutation The current order of pins (nodes).
 * @param desired_permutation The desired order of pins (nodes).
 * @return The maximum number of rows required.
 */
template <typename Lyt>
uint32_t calculate_rows_needed(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& current_permutation,
                               const std::vector<mockturtle::node<Lyt>>& desired_permutation)
{
    if (desired_permutation.empty() || current_permutation.empty())
    {
        return 0;
    }

    const auto current_coords = determine_pin_coordinates(lyt, current_permutation);

    uint32_t max_rows = 0;

    for (size_t i = 0; i < desired_permutation.size(); ++i)
    {
        const auto target_node = desired_permutation[i];

        // Find where this node is currently located
        const auto it = std::find(current_permutation.cbegin(), current_permutation.cend(), target_node);
        if (it == current_permutation.end())
        {
            continue;
        }

        // Current location of the node
        const auto current_idx  = static_cast<size_t>(std::distance(current_permutation.cbegin(), it));
        const auto source_coord = current_coords[current_idx];

        // Target x-coordinate is the x-coordinate of the i-th slot (from current_coords[i])
        const auto target_x = current_coords[i].x;

        const auto dist =
            static_cast<uint32_t>(std::abs(static_cast<int32_t>(source_coord.x) - static_cast<int32_t>(target_x)));

        // Hexagonal constraint: 2 rows per 1 unit of horizontal distance.
        const uint32_t rows = dist * 2;

        max_rows = std::max(rows, max_rows);
    }

    return max_rows;
}

}  // namespace detail

/**
 * A physical design algorithm to unscramble input and output pin orderings in placed and routed pointy-top row-wise
 * clocked hexagonal gate-level layouts.
 *
 * This algorithm takes an existing gate-level layout and reorders its primary inputs and outputs according to a
 * specified target ordering.
 *
 * @note This significantly increases the area footprint of the layout.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The gate-level layout to unscramble.
 * @param input_order The desired ordering of primary input nodes.
 * @param output_order The desired ordering of primary output nodes.
 * @param ps Parameters for the algorithm.
 * @param pst Statistics for the algorithm.
 * @return A new gate-level layout with unscrambled pins.
 */
template <typename Lyt>
Lyt unscramble_pins(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& input_order,
                    const std::vector<mockturtle::node<Lyt>>& output_order, unscramble_pins_params ps = {},
                    unscramble_pins_stats* pst = nullptr)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");
    static_assert(is_hexagonal_layout_v<Lyt>, "Lyt is not a hexagonal layout");
    static_assert(has_pointy_top_hex_orientation_v<Lyt>, "Lyt does not have pointy-top hexagonal orientation");

    assert(lyt.is_clocking_scheme("ROW") && "Layout must be row-wise clocked");

    // Placeholder
    return lyt.clone();
}

}  // namespace fiction

#endif  // FICTION_UNSCRAMBLE_PINS_HPP
