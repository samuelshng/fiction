//
// Created by simon on 25.09.23.
//

#ifndef FICTION_WRITE_FGL_LAYOUT_HPP
#define FICTION_WRITE_FGL_LAYOUT_HPP

#include "fiction/traits.hpp"
#include "fiction/utils/stl_utils.hpp"
#include "fiction/utils/version_info.hpp"

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <kitty/print.hpp>
#include <mockturtle/views/topo_view.hpp>

#include <cstdint>
#include <ctime>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>

namespace fiction
{

namespace detail
{

namespace fgl
{

inline constexpr auto FGL_HEADER       = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
inline constexpr auto OPEN_FGL         = "<fgl>\n";
inline constexpr auto CLOSE_FGL        = "</fgl>\n";
inline constexpr auto FICTION_METADATA = "  <fiction>\n"
                                         "    <fiction_version>{}</fiction_version>\n"
                                         "    <available_at>{}</available_at>\n"
                                         "    <date>{}</date>\n"
                                         "  </fiction>\n";

inline constexpr auto OPEN_LAYOUT_METADATA  = "  <layout>\n";
inline constexpr auto CLOSE_LAYOUT_METADATA = "  </layout>\n";
inline constexpr auto LAYOUT_METADATA       = "    <name>{}</name>\n"
                                              "    <topology>{}</topology>\n"
                                              "    <size>\n"
                                              "      <x>{}</x>\n"
                                              "      <y>{}</y>\n"
                                              "      <z>{}</z>\n"
                                              "    </size>\n";
inline constexpr auto OPEN_CLOCKING         = "    <clocking>\n";
inline constexpr auto CLOSE_CLOCKING        = "    </clocking>\n";
inline constexpr auto CLOCKING_SCHEME_NAME  = "      <name>{}</name>\n";
inline constexpr auto OPEN_CLOCK_ZONES      = "      <zones>\n";
inline constexpr auto CLOSE_CLOCK_ZONES     = "      </zones>\n";
inline constexpr auto CLOCK_ZONE            = "        <zone>\n"
                                              "          <x>{}</x>\n"
                                              "          <y>{}</y>\n"
                                              "          <clock>{}</clock>\n"
                                              "        </zone>\n";

inline constexpr auto OPEN_GATES     = "  <gates>\n";
inline constexpr auto CLOSE_GATES    = "  </gates>\n";
inline constexpr auto OPEN_GATE      = "    <gate>\n";
inline constexpr auto CLOSE_GATE     = "    </gate>\n";
inline constexpr auto GATE           = "      <id>{}</id>\n"
                                       "      <type>{}</type>\n"
                                       "      <name>{}</name>\n"
                                       "      <loc>\n"
                                       "        <x>{}</x>\n"
                                       "        <y>{}</y>\n"
                                       "        <z>{}</z>\n"
                                       "      </loc>\n";
inline constexpr auto OPEN_INCOMING  = "      <incoming>\n";
inline constexpr auto CLOSE_INCOMING = "      </incoming>\n";
inline constexpr auto SIGNAL         = "        <signal>\n"
                                       "          <x>{}</x>\n"
                                       "          <y>{}</y>\n"
                                       "          <z>{}</z>\n"
                                       "          <p>{}</p>\n"
                                       "        </signal>\n";

}  // namespace fgl

template <typename Lyt>
class write_fgl_layout_impl
{
  public:
    write_fgl_layout_impl(const Lyt& src, std::ostream& s) : lyt{src}, os{s} {}

    void run()
    {
        // metadata
        os << fgl::FGL_HEADER << fgl::OPEN_FGL;
        const auto current_time = std::time(nullptr);
        const auto time_str     = fmt::format("{:%Y-%m-%d %H:%M:%S}", safe_localtime(current_time));
        os << fmt::format(fgl::FICTION_METADATA, FICTION_VERSION, FICTION_REPO, time_str);

        os << fgl::OPEN_LAYOUT_METADATA;
        std::string layout_name = get_name(lyt);

        // check if topology matches Lyt
        std::string topology{};
        if constexpr (is_cartesian_layout_v<Lyt>)
        {
            topology = "cartesian";
        }
        else if constexpr (is_shifted_cartesian_layout_v<Lyt>)
        {
            if constexpr (has_odd_row_cartesian_arrangement_v<Lyt>)
            {
                topology = "odd_row_cartesian";
            }
            else if constexpr (has_even_row_cartesian_arrangement_v<Lyt>)
            {
                topology = "even_row_cartesian";
            }
            else if constexpr (has_odd_column_cartesian_arrangement_v<Lyt>)
            {
                topology = "odd_column_cartesian";
            }
            else if constexpr (has_even_column_cartesian_arrangement_v<Lyt>)
            {
                topology = "even_column_cartesian";
            }
        }
        else if constexpr (is_hexagonal_layout_v<Lyt>)
        {
            if constexpr (has_odd_row_hex_arrangement_v<Lyt>)
            {
                topology = "odd_row_hex";
            }
            else if constexpr (has_even_row_hex_arrangement_v<Lyt>)
            {
                topology = "even_row_hex";
            }
            else if constexpr (has_odd_column_hex_arrangement_v<Lyt>)
            {
                topology = "odd_column_hex";
            }
            else if constexpr (has_even_column_hex_arrangement_v<Lyt>)
            {
                topology = "even_column_hex";
            }
        }

        os << fmt::format(fgl::LAYOUT_METADATA, layout_name, topology, lyt.x(), lyt.y(), lyt.z());

        os << fgl::OPEN_CLOCKING;
        const auto clocking_scheme = lyt.get_clocking_scheme();
        os << fmt::format(fgl::CLOCKING_SCHEME_NAME, clocking_scheme.name);

        // if clocking scheme is irregular, overwrite clock zones
        if (!clocking_scheme.is_regular())
        {
            os << fgl::OPEN_CLOCK_ZONES;
            for (uint64_t x = 0; x <= lyt.x(); ++x)
            {
                for (uint64_t y = 0; y <= lyt.y(); ++y)
                {
                    int clock = clocking_scheme({x, y});
                    os << fmt::format(fgl::CLOCK_ZONE, x, y, clock);
                }
            }
            os << fgl::CLOSE_CLOCK_ZONES;
        }
        os << fgl::CLOSE_CLOCKING;
        os << fgl::CLOSE_LAYOUT_METADATA;

        os << fgl::OPEN_GATES;

        // create topological ordering
        mockturtle::topo_view layout_topo{lyt};
        uint32_t              gate_id = 0;

        // inputs
        layout_topo.foreach_pi(
            [&gate_id, this](const auto& gate)
            {
                const auto coord = lyt.get_tile(gate);
                os << fgl::OPEN_GATE;
                os << fmt::format(fgl::GATE, gate_id, "PI", lyt.get_name(gate), coord.x, coord.y, coord.z);
                os << fgl::CLOSE_GATE;
                gate_id++;
            });

        // gates
        layout_topo.foreach_gate(
            [&gate_id, this](const auto& gate)
            {
                os << fgl::OPEN_GATE;
                const auto                           coord = lyt.get_tile(gate);
                std::vector<mockturtle::signal<Lyt>> signals{};
                signals.reserve(lyt.fanin_size(gate));
                lyt.foreach_fanin(gate, [&signals](const auto& signal) { signals.push_back(signal); });

                const auto signal_to_tile = [](const auto& signal) { return static_cast<tile<Lyt>>(signal); };

                if (signals.size() == 1)
                {
                    const auto incoming_signal = signal_to_tile(signals[0]);

                    if (lyt.is_po(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "PO", lyt.get_name(gate), coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_wire(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "BUF", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_inv(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "INV", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_function(gate))
                    {
                        const auto node_fun = lyt.node_function(gate);

                        os << fmt::format(fgl::GATE, gate_id, kitty::to_hex(node_fun), "", coord.x, coord.y, coord.z);
                    }

                    os << fgl::OPEN_INCOMING;
                    os << fmt::format(fgl::SIGNAL, incoming_signal.x, incoming_signal.y, incoming_signal.z,
                                      signals[0].output);
                    os << fgl::CLOSE_INCOMING;
                }
                else if (signals.size() == 2)
                {
                    const auto incoming_signal_a = signal_to_tile(signals[0]);
                    const auto incoming_signal_b = signal_to_tile(signals[1]);

                    if constexpr (has_is_ha_v<Lyt>)
                    {
                        if (lyt.is_ha(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "HA", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_and(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "AND", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_nand(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "NAND", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_or(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "OR", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_nor(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "NOR", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_xor(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "XOR", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_xnor(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "XNOR", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_lt(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "LT", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_gt(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "GT", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_le(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "LE", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_ge(gate))
                        {
                            os << fmt::format(fgl::GATE, gate_id, "GE", "", coord.x, coord.y, coord.z);
                        }
                        else if (lyt.is_function(gate))
                        {
                            const auto node_fun = lyt.node_function(gate);

                            os << fmt::format(fgl::GATE, gate_id, kitty::to_hex(node_fun), "", coord.x, coord.y,
                                              coord.z);
                        }
                    }
                    else if (lyt.is_and(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "AND", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_nand(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "NAND", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_or(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "OR", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_nor(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "NOR", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_xor(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "XOR", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_xnor(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "XNOR", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_lt(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "LT", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_gt(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "GT", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_le(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "LE", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_ge(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "GE", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_function(gate))
                    {
                        const auto node_fun = lyt.node_function(gate);

                        os << fmt::format(fgl::GATE, gate_id, kitty::to_hex(node_fun), "", coord.x, coord.y, coord.z);
                    }
                    os << fgl::OPEN_INCOMING;
                    os << fmt::format(fgl::SIGNAL, incoming_signal_a.x, incoming_signal_a.y, incoming_signal_a.z,
                                      signals[0].output);
                    os << fmt::format(fgl::SIGNAL, incoming_signal_b.x, incoming_signal_b.y, incoming_signal_b.z,
                                      signals[1].output);
                    os << fgl::CLOSE_INCOMING;
                }
                else if (signals.size() == 3)
                {
                    const auto incoming_signal_a = signal_to_tile(signals[0]);
                    const auto incoming_signal_b = signal_to_tile(signals[1]);
                    const auto incoming_signal_c = signal_to_tile(signals[2]);

                    if (lyt.is_maj(gate))
                    {
                        os << fmt::format(fgl::GATE, gate_id, "MAJ", "", coord.x, coord.y, coord.z);
                    }
                    else if (lyt.is_function(gate))
                    {
                        const auto node_fun = lyt.node_function(gate);

                        os << fmt::format(fgl::GATE, gate_id, kitty::to_hex(node_fun), "", coord.x, coord.y, coord.z);
                    }
                    os << fgl::OPEN_INCOMING;
                    os << fmt::format(fgl::SIGNAL, incoming_signal_a.x, incoming_signal_a.y, incoming_signal_a.z,
                                      signals[0].output);
                    os << fmt::format(fgl::SIGNAL, incoming_signal_b.x, incoming_signal_b.y, incoming_signal_b.z,
                                      signals[1].output);
                    os << fmt::format(fgl::SIGNAL, incoming_signal_c.x, incoming_signal_c.y, incoming_signal_c.z,
                                      signals[2].output);
                    os << fgl::CLOSE_INCOMING;
                }
                else if (lyt.is_function(gate))
                {
                    const auto node_fun = lyt.node_function(gate);

                    os << fmt::format(fgl::GATE, gate_id, kitty::to_hex(node_fun), "", coord.x, coord.y, coord.z);

                    os << fgl::OPEN_INCOMING;
                    for (std::size_t i = 0; i < signals.size(); i++)
                    {
                        const auto incoming_signal = signal_to_tile(signals[i]);
                        os << fmt::format(fgl::SIGNAL, incoming_signal.x, incoming_signal.y, incoming_signal.z,
                                          signals[i].output);
                    }
                    os << fgl::CLOSE_INCOMING;
                }
                os << fgl::CLOSE_GATE;
                gate_id++;
            });

        os << fgl::CLOSE_GATES;
        os << fgl::CLOSE_FGL;
    }

  private:
    /**
     * The layout to be written.
     */
    Lyt lyt;
    /**
     * The output stream to which the gate-level layout is written.
     */
    std::ostream& os;
};

}  // namespace detail

/**
 * Writes an FGL layout to a file.
 *
 * This overload uses an output stream to write into.
 *
 * @tparam Lyt Layout.
 * @param lyt The layout to be written.
 * @param os The output stream to write into.
 */
template <typename Lyt>
void write_fgl_layout(const Lyt& lyt, std::ostream& os)
{
    static_assert(is_gate_level_layout_v<Lyt>, "Lyt is not a gate-level layout");

    detail::write_fgl_layout_impl p{lyt, os};

    p.run();
}
/**
 * Writes an FGL layout to a file.
 *
 * This overload uses a file name to create and write into.
 *
 * @tparam Lyt Layout.
 * @param lyt The layout to be written.
 * @param filename The file name to create and write into. Should preferably use the .fgl extension.
 */
template <typename Lyt>
void write_fgl_layout(const Lyt& lyt, const std::string_view& filename)
{
    std::ofstream os{std::string{filename}, std::ofstream::out};

    if (!os.is_open())
    {
        throw std::ofstream::failure("could not open file");
    }

    write_fgl_layout(lyt, os);
    os.close();
}

}  // namespace fiction

#endif  // FICTION_WRITE_FGL_LAYOUT_HPP
