/**
 * @file hex_layout_port_legality.hpp
 * @brief Test utilities for validating projected port usage in pointy-top hexagonal gate layouts.
 */

#ifndef FICTION_TEST_UTILS_HEX_LAYOUT_PORT_LEGALITY_HPP
#define FICTION_TEST_UTILS_HEX_LAYOUT_PORT_LEGALITY_HPP

#include <fiction/layouts/hexagonal_layout.hpp>
#include <fiction/traits.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace test::hex_layout_port_legality
{

/**
 * @brief Relative projected port sides of a pointy-top hex tile.
 */
enum class projected_port_side : uint8_t
{
    north_west,
    north_east,
    south_west,
    south_east
};

/**
 * @brief Aggregated projected-port usage for one hex tile across all occupied z layers.
 */
struct projected_port_usage
{
    /**
     * @brief Number of occupied nodes stacked on the same projected tile.
     */
    uint32_t occupant_count{};
    /**
     * @brief Whether all occupants are wires/fanouts.
     */
    bool only_wires = true;
    /**
     * @brief Number of incoming projected connections per side.
     */
    std::array<uint32_t, 4> incoming_counts{};
    /**
     * @brief Number of outgoing projected connections per side.
     */
    std::array<uint32_t, 4> outgoing_counts{};
};

/**
 * @brief Encodes a projected `(x, y)` tile coordinate into a hashable integer key.
 *
 * @tparam Coordinate Coordinate type with `x` and `y` members.
 * @param t Projected tile coordinate.
 * @return Integer key for hash tables.
 */
template <typename Coordinate>
[[nodiscard]] uint64_t projected_key(const Coordinate& t) noexcept
{
    return (static_cast<uint64_t>(t.x) << 32ULL) | static_cast<uint64_t>(t.y);
}

/**
 * @brief Returns the ground-layer projection of a tile.
 *
 * @tparam Lyt Layout type.
 * @param lyt Layout instance.
 * @param t Tile to project.
 * @return Ground-layer tile with the same `(x, y)`.
 */
template <typename Lyt>
[[nodiscard]] fiction::tile<Lyt> projected_tile(const Lyt& lyt, const fiction::tile<Lyt>& t) noexcept
{
    return lyt.below(t);
}

/**
 * @brief Converts a projected port side into a human-readable short name.
 *
 * @param side Port side to stringify.
 * @return Side name.
 */
[[nodiscard]] inline const char* to_string(const projected_port_side side) noexcept
{
    switch (side)
    {
        case projected_port_side::north_west: return "NW";
        case projected_port_side::north_east: return "NE";
        case projected_port_side::south_west: return "SW";
        case projected_port_side::south_east: return "SE";
    }

    return "?";
}

/**
 * @brief Maps an incoming projected neighbor to its corresponding pointy-top side.
 *
 * @tparam Lyt Layout type.
 * @param lyt Layout instance.
 * @param current Current tile.
 * @param neighbor Incoming neighboring tile.
 * @return Side of the incoming neighbor if it is a legal top-side neighbor.
 */
template <typename Lyt>
[[nodiscard]] std::optional<projected_port_side> incoming_side(const Lyt& lyt, const fiction::tile<Lyt>& current,
                                                               const fiction::tile<Lyt>& neighbor) noexcept
{
    const auto base_current  = projected_tile(lyt, current);
    const auto base_neighbor = projected_tile(lyt, neighbor);

    if (lyt.north_west(base_current) == base_neighbor)
    {
        return projected_port_side::north_west;
    }

    if (lyt.north_east(base_current) == base_neighbor)
    {
        return projected_port_side::north_east;
    }

    return std::nullopt;
}

/**
 * @brief Maps an outgoing projected neighbor to its corresponding pointy-top side.
 *
 * @tparam Lyt Layout type.
 * @param lyt Layout instance.
 * @param current Current tile.
 * @param neighbor Outgoing neighboring tile.
 * @return Side of the outgoing neighbor if it is a legal bottom-side neighbor.
 */
template <typename Lyt>
[[nodiscard]] std::optional<projected_port_side> outgoing_side(const Lyt& lyt, const fiction::tile<Lyt>& current,
                                                               const fiction::tile<Lyt>& neighbor) noexcept
{
    const auto base_current  = projected_tile(lyt, current);
    const auto base_neighbor = projected_tile(lyt, neighbor);

    if (lyt.south_west(base_current) == base_neighbor)
    {
        return projected_port_side::south_west;
    }

    if (lyt.south_east(base_current) == base_neighbor)
    {
        return projected_port_side::south_east;
    }

    return std::nullopt;
}

/**
 * @brief Collects strict projected-port legality violations for a pointy-top hex layout.
 *
 * The check is intentionally stricter than the current built-in DRV checks. All occupied nodes that project to the
 * same `(x, y)` tile are treated as one composite pointy-top tile. For that composite tile, each top side (`NW`, `NE`)
 * may accept at most one incoming connection and each bottom side (`SW`, `SE`) may emit at most one outgoing
 * connection. Stacked projected tiles are additionally required to contain wires only.
 *
 * @tparam Lyt Layout type.
 * @param lyt Layout to inspect.
 * @return Human-readable violation messages. Empty means no violations were found.
 */
template <typename Lyt>
[[nodiscard]] std::vector<std::string> collect_port_violations(const Lyt& lyt)
{
    static_assert(fiction::is_hexagonal_layout_v<Lyt>, "Lyt must be a hexagonal layout");
    static_assert(fiction::has_pointy_top_hex_orientation_v<Lyt>, "Lyt must use pointy-top hexagonal orientation");

    std::unordered_map<uint64_t, projected_port_usage> usage_by_tile{};
    std::vector<std::string>                           violations{};

    lyt.foreach_node(
        [&lyt, &usage_by_tile](const auto& node)
        {
            if (lyt.is_constant(node))
            {
                return;
            }

            const auto current_tile = lyt.get_tile(node);
            const auto key          = projected_key(projected_tile(lyt, current_tile));
            auto&      usage        = usage_by_tile[key];

            ++usage.occupant_count;
            usage.only_wires = usage.only_wires && lyt.is_wire(node);

            for (const auto& incoming : lyt.incoming_data_flow(current_tile))
            {
                if (const auto side = incoming_side(lyt, current_tile, static_cast<fiction::tile<Lyt>>(incoming));
                    side.has_value())
                {
                    ++usage.incoming_counts[static_cast<uint8_t>(*side)];
                }
            }

            for (const auto& outgoing : lyt.outgoing_data_flow(current_tile))
            {
                if (const auto side = outgoing_side(lyt, current_tile, static_cast<fiction::tile<Lyt>>(outgoing));
                    side.has_value())
                {
                    ++usage.outgoing_counts[static_cast<uint8_t>(*side)];
                }
            }
        });

    lyt.foreach_ground_tile(
        [&lyt, &usage_by_tile, &violations](const auto& ground_tile)
        {
            const auto key = projected_key(ground_tile);

            if (const auto it = usage_by_tile.find(key); it != usage_by_tile.cend())
            {
                const auto& usage = it->second;

                if (usage.occupant_count > 1u && !usage.only_wires)
                {
                    std::ostringstream os{};
                    os << "projected tile (" << ground_tile.x << ", " << ground_tile.y
                       << ") stacks non-wire occupants across z layers";
                    violations.push_back(os.str());
                }

                for (const auto side : {projected_port_side::north_west, projected_port_side::north_east})
                {
                    const auto count = usage.incoming_counts[static_cast<uint8_t>(side)];

                    if (count > 1u)
                    {
                        std::ostringstream os{};
                        os << "projected tile (" << ground_tile.x << ", " << ground_tile.y << ") uses incoming side "
                           << to_string(side) << ' ' << count << " times";
                        violations.push_back(os.str());
                    }
                }

                for (const auto side : {projected_port_side::south_west, projected_port_side::south_east})
                {
                    const auto count = usage.outgoing_counts[static_cast<uint8_t>(side)];

                    if (count > 1u)
                    {
                        std::ostringstream os{};
                        os << "projected tile (" << ground_tile.x << ", " << ground_tile.y << ") uses outgoing side "
                           << to_string(side) << ' ' << count << " times";
                        violations.push_back(os.str());
                    }
                }
            }
        });

    return violations;
}

}  // namespace test::hex_layout_port_legality

#endif  // FICTION_TEST_UTILS_HEX_LAYOUT_PORT_LEGALITY_HPP
