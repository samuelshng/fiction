//
// Created by marcel on 10.02.26.
//

#ifndef FICTION_UNSCRAMBLE_PINS_HPP
#define FICTION_UNSCRAMBLE_PINS_HPP

#include "fiction/layouts/clocking_scheme.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/network_utils.hpp"
#include "fiction/utils/placement_utils.hpp"
#include "fiction/utils/routing_utils.hpp"

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
 * Calculates the horizontal permutation distances for each desired slot.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The layout.
 * @param current_permutation The current order of pins (nodes).
 * @param desired_permutation The desired order of pins (nodes).
 * @return Vector of horizontal distances per desired slot.
 */
template <typename Lyt>
std::vector<uint32_t> calculate_permutation_distances(const Lyt&                                lyt,
                                                      const std::vector<mockturtle::node<Lyt>>& current_permutation,
                                                      const std::vector<mockturtle::node<Lyt>>& desired_permutation)
{
    std::vector<uint32_t> distances(desired_permutation.size(), 0u);

    if (desired_permutation.empty() || current_permutation.empty())
    {
        return distances;
    }

    const auto current_coords = determine_pin_coordinates(lyt, current_permutation);

    for (size_t i = 0; i < desired_permutation.size(); ++i)
    {
        if (i >= current_coords.size())
        {
            break;
        }

        const auto target_node = desired_permutation[i];

        // Find where this node is currently located
        const auto it = std::find(current_permutation.cbegin(), current_permutation.cend(), target_node);
        if (it == current_permutation.cend())
        {
            continue;
        }

        // Current location of the node
        const auto current_idx  = static_cast<size_t>(std::distance(current_permutation.cbegin(), it));
        const auto source_coord = current_coords[current_idx];

        // Target x-coordinate is the x-coordinate of the i-th slot (from current_coords[i])
        const auto target_x = current_coords[i].x;

        distances[i] =
            static_cast<uint32_t>(std::abs(static_cast<int32_t>(source_coord.x) - static_cast<int32_t>(target_x)));
    }

    return distances;
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

    const auto distances = calculate_permutation_distances(lyt, current_permutation, desired_permutation);

    uint32_t max_rows = 0;

    for (const auto dist : distances)
    {
        // Hexagonal constraint: 2 rows per 1 unit of horizontal distance.
        const uint32_t rows = dist * 2;

        max_rows = std::max(rows, max_rows);
    }

    return max_rows;
}

/**
 * Creates a new layout with space reserved for PI/PO unscrambling rows.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The original layout.
 * @param pi_rows Rows reserved for input unscrambling.
 * @param po_rows Rows reserved for output unscrambling.
 * @return New layout with extended height and original clocking/name.
 */
template <typename Lyt>
Lyt create_extended_layout(const Lyt& lyt, const uint32_t pi_rows, const uint32_t po_rows)
{
    const aspect_ratio<Lyt> new_ar{lyt.x(), lyt.y() + pi_rows + po_rows, lyt.z()};
    return Lyt{new_ar, lyt.get_clocking_scheme(), lyt.get_layout_name()};
}
/**
 * Places new PIs in the top row and creates routing objectives to their original locations (shifted by pi_rows). The
 * objectives are sorted by their horizontal permutation distance to prioritize longer routes first.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt The original layout.
 * @param new_layout The new layout to place PIs in.
 * @param current_pis The current PI ordering.
 * @param desired_pis The desired PI ordering.
 * @param pi_rows Rows reserved for input unscrambling.
 * @return Routing objectives sorted by permutation distance.
 */
template <typename Lyt>
std::vector<routing_objective<Lyt>>
create_pi_routing_objectives(const Lyt& lyt, Lyt& new_layout, const std::vector<mockturtle::node<Lyt>>& current_pis,
                             const std::vector<mockturtle::node<Lyt>>& desired_pis, const uint32_t pi_rows)
{
    // Current PI coordinates define the slot x-positions for the desired ordering.
    const auto current_pi_coords = determine_pin_coordinates(lyt, current_pis);
    // Precompute horizontal distances to sort objectives by route length.
    const auto pi_distances = calculate_permutation_distances(lyt, current_pis, desired_pis);

    std::vector<std::pair<uint32_t, routing_objective<Lyt>>> pi_objectives{};
    pi_objectives.reserve(desired_pis.size());

    for (size_t i = 0; i < desired_pis.size(); ++i)
    {
        if (i >= current_pi_coords.size())
        {
            break;
        }

        const auto desired_node = desired_pis[i];
        const auto slot_x       = current_pi_coords[i].x;

        // Place new PI in the top row of the new layout at the desired x-coordinate.
        const tile<Lyt> source{slot_x, 0};
        const auto      pi_name = lyt.get_name(desired_node);
        new_layout.create_pi(pi_name, source);

        // Route to the original PI coordinate, shifted down by the PI routing rows.
        const auto      original_coord = lyt.get_tile(desired_node);
        const tile<Lyt> target{original_coord.x, original_coord.y + pi_rows, original_coord.z};

        pi_objectives.push_back({pi_distances[i], {source, target}});
    }

    // Sort objectives by distance (descending) to prioritize longer routes first.
    std::sort(pi_objectives.begin(), pi_objectives.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });

    // Extract routing objectives from the sorted (distance, objective) pairs.
    std::vector<routing_objective<Lyt>> pi_routing_objectives{};
    pi_routing_objectives.reserve(pi_objectives.size());
    std::transform(pi_objectives.cbegin(), pi_objectives.cend(), std::back_inserter(pi_routing_objectives),
                   [](const auto& entry) { return entry.second; });

    return pi_routing_objectives;
}

template <typename Lyt>
class unscramble_pins_impl
{
  public:
    unscramble_pins_impl(const Lyt& lyt, const std::vector<mockturtle::node<Lyt>>& input_order,
                         const std::vector<mockturtle::node<Lyt>>& output_order, const unscramble_pins_params& p,
                         unscramble_pins_stats& st) :
            layout{lyt},
            input_ordering{input_order},
            output_ordering{output_order},
            params{p},
            pst{st}
    {}

    Lyt run()
    {
        mockturtle::stopwatch stop{pst.time_total};

        // 1. Identify current pin orderings
        const auto current_pis = get_current_pis();
        const auto current_pos = get_current_pos();

        // 2. Calculate unscrambling space requirements
        const uint32_t pi_rows = calculate_rows_needed(layout, current_pis, input_ordering);
        const uint32_t po_rows = calculate_rows_needed(layout, current_pos, output_ordering);

        // 3. Instantiate new layout with extended height
        auto new_layout = create_extended_layout(layout, pi_rows, po_rows);

        // 4. Place new PIs and create routing objectives to the original PI locations (shifted by pi_rows)
        [[maybe_unused]] const auto pi_routing_objectives =
            create_pi_routing_objectives(layout, new_layout, current_pis, input_ordering, pi_rows);

        // Placeholder for the rest of the implementation
        return layout;
    }

  private:
    /**
     * The original layout to unscramble.
     */
    const Lyt& layout;
    /**
     * The desired ordering of primary input nodes.
     */
    std::vector<mockturtle::node<Lyt>> input_ordering;
    /**
     * The desired ordering of primary output nodes.
     */
    std::vector<mockturtle::node<Lyt>> output_ordering;
    /**
     * Parameters for the pin unscrambling algorithm.
     */
    unscramble_pins_params params;
    /**
     * Statistics for the pin unscrambling algorithm.
     */
    unscramble_pins_stats& pst;

    /**
     * Identifies the current primary input nodes in the layout.
     *
     * @return Vector of primary input nodes.
     */
    std::vector<mockturtle::node<Lyt>> get_current_pis() const
    {
        std::vector<mockturtle::node<Lyt>> pis{};
        pis.reserve(layout.num_pis());
        layout.foreach_pi([&pis](const auto& pi) { pis.push_back(pi); });

        return pis;
    }
    /**
     * Identifies the current primary output nodes in the layout.
     *
     * @return Vector of primary output nodes.
     */
    std::vector<mockturtle::node<Lyt>> get_current_pos() const
    {
        std::vector<mockturtle::node<Lyt>> pos{};
        pos.reserve(layout.num_pos());
        layout.foreach_po([&pos, this](const auto& po) { pos.push_back(layout.get_node(po)); });

        return pos;
    }
};

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

    unscramble_pins_stats             st{};
    detail::unscramble_pins_impl<Lyt> p{lyt, input_order, output_order, ps, st};

    const auto result = p.run();

    if (pst)
    {
        *pst = st;
    }

    return result;
}

}  // namespace fiction

#endif  // FICTION_UNSCRAMBLE_PINS_HPP
