//
// Created by marcel on 04.03.20.
//

#ifndef FICTION_EQUIVALENCE_CHECKING_HPP
#define FICTION_EQUIVALENCE_CHECKING_HPP

#include "fiction/algorithms/properties/critical_path_length_and_throughput.hpp"
#include "fiction/algorithms/verification/design_rule_violations.hpp"
#include "fiction/traits.hpp"
#include "fiction/utils/name_utils.hpp"

#include <mockturtle/algorithms/equivalence_checking.hpp>
#include <mockturtle/algorithms/miter.hpp>
#include <mockturtle/networks/klut.hpp>
#include <mockturtle/traits.hpp>
#include <mockturtle/utils/stopwatch.hpp>
#include <mockturtle/views/topo_view.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>

namespace fiction
{
/**
 * The different equivalence types possible.
 */
enum class eq_type : uint8_t
{
    /**
     * `Spec` and `Impl` are logically not equivalent OR `Impl` has DRVs.
     */
    NO,
    /**
     * `Spec` and `Impl` are logically equivalent BUT `Impl` has a throughput of \f$\frac{1}{x}\f$ with \f$x > 1\f$.
     */
    WEAK,
    /**
     * `Spec` and `Impl` are logically equivalent AND `Impl` has a throughput of \f$\frac{1}{1}\f$.
     */
    STRONG
};

struct equivalence_checking_stats
{
    /**
     * Stores the equivalence type.
     */
    eq_type eq = eq_type::NO;
    /**
     * Throughput values at which weak equivalence manifests.
     */
    int64_t tp_spec = 1ll, tp_impl = 1ll, tp_diff = 0ll;
    /**
     * Stores a possible counter example.
     */
    std::vector<bool> counter_example{};
    /**
     * Stores the runtime.
     */
    mockturtle::stopwatch<>::duration runtime{0};
    /**
     * Stores DRVs.
     */
    fiction::gate_level_drv_stats spec_drv_stats{}, impl_drv_stats{};
};

namespace detail
{

/**
 * @brief Returns a signal's output pin if present, otherwise pin 0.
 *
 * @tparam Ntk Network type.
 * @param s Network signal.
 * @return Output pin index represented by `s`.
 */
template <typename Ntk>
[[nodiscard]] uint32_t signal_output_pin([[maybe_unused]] const mockturtle::signal<Ntk>& s) noexcept
{
    if constexpr (has_signal_output_pin_v<Ntk>)
    {
        return static_cast<uint32_t>(s.output);
    }

    return 0u;
}

/**
 * @brief Returns the number of output pins for a node.
 *
 * @tparam Ntk Network type.
 * @param ntk Network instance.
 * @param n Node in `ntk`.
 * @return Number of output pins represented by `n`.
 */
template <typename Ntk>
[[nodiscard]] uint32_t node_output_pin_count(const Ntk& ntk, const mockturtle::node<Ntk>& n) noexcept
{
    if constexpr (mockturtle::has_num_outputs_v<Ntk>)
    {
        if (ntk.is_multioutput(n))
        {
            return ntk.num_outputs(n);
        }
    }

    if (ntk.is_multioutput(n))
    {
        return 2u;
    }

    return 1u;
}

/**
 * @brief Ensures bookkeeping vectors can represent all output pins of a node.
 *
 * @tparam Ntk Network type.
 * @param old2new Node mapping from source network signals to resulting KLUT signals.
 * @param required_outputs Marker vector for output pins that are required in the resulting network.
 * @param ntk Source network.
 * @param n Node to reserve output-pin capacity for.
 */
template <typename Ntk>
void ensure_output_pin_capacity(mockturtle::node_map<std::vector<mockturtle::signal<mockturtle::klut_network>>,
                                                     mockturtle::topo_view<Ntk>>&                    old2new,
                                mockturtle::node_map<std::vector<bool>, mockturtle::topo_view<Ntk>>& required_outputs,
                                const mockturtle::topo_view<Ntk>& ntk, const typename Ntk::node n)
{
    const auto pin_count = node_output_pin_count(ntk, n);

    if (old2new[n].size() < pin_count)
    {
        old2new[n].resize(pin_count);
    }
    if (required_outputs[n].size() < pin_count)
    {
        required_outputs[n].resize(pin_count, false);
    }
}

/**
 * @brief Ensures bookkeeping vectors can represent a specific output pin of a node.
 *
 * @tparam Ntk Network type.
 * @param old2new Node mapping from source network signals to resulting kLUT signals.
 * @param required_outputs Marker vector for output pins that are required in the resulting network.
 * @param ntk Source network.
 * @param n Node to reserve output-pin capacity for.
 * @param pin Output pin to reserve.
 */
template <typename Ntk>
void ensure_output_pin_capacity(mockturtle::node_map<std::vector<mockturtle::signal<mockturtle::klut_network>>,
                                                     mockturtle::topo_view<Ntk>>&                    old2new,
                                mockturtle::node_map<std::vector<bool>, mockturtle::topo_view<Ntk>>& required_outputs,
                                const mockturtle::topo_view<Ntk>& ntk, const typename Ntk::node n, const uint32_t pin)
{
    ensure_output_pin_capacity(old2new, required_outputs, ntk, n);

    if (old2new[n].size() <= pin)
    {
        old2new[n].resize(pin + 1u);
    }
    if (required_outputs[n].size() <= pin)
    {
        required_outputs[n].resize(pin + 1u, false);
    }
}

/**
 * @brief Splits all multi-output nodes in a network into dedicated single-output kLUT nodes.
 *
 * @tparam Ntk Source network type.
 * @param src Source network.
 * @return Equivalent single-output `klut_network`.
 */
template <typename Ntk>
mockturtle::klut_network split_multioutput_network(const Ntk& src)
{
    static_assert(mockturtle::has_is_multioutput_v<Ntk>, "Ntk does not implement is_multioutput");
    static_assert(mockturtle::has_node_function_pin_v<Ntk>, "Ntk does not implement node_function_pin");

    using klut_signal = mockturtle::signal<mockturtle::klut_network>;

    mockturtle::topo_view<Ntk>                                                 topo_ntk{src};
    mockturtle::klut_network                                                   klut_ntk{};
    mockturtle::node_map<std::vector<klut_signal>, mockturtle::topo_view<Ntk>> old2new{topo_ntk};
    mockturtle::node_map<std::vector<bool>, mockturtle::topo_view<Ntk>>        required_outputs{topo_ntk};

    topo_ntk.foreach_gate(
        [&topo_ntk, &old2new, &required_outputs](const auto& gate)
        {
            ensure_output_pin_capacity(old2new, required_outputs, topo_ntk, gate);

            topo_ntk.foreach_fanin(gate,
                                   [&topo_ntk, &old2new, &required_outputs](const auto& fanin)
                                   {
                                       const auto fanin_node = topo_ntk.get_node(fanin);
                                       if (!topo_ntk.is_constant(fanin_node))
                                       {
                                           const auto output_pin = static_cast<std::size_t>(
                                               topo_ntk.is_multioutput(fanin_node) ? signal_output_pin<Ntk>(fanin) :
                                                                                     0u);
                                           ensure_output_pin_capacity(old2new, required_outputs, topo_ntk, fanin_node,
                                                                      static_cast<uint32_t>(output_pin));
                                           required_outputs[fanin_node][output_pin] = true;
                                       }
                                   });
        });

    topo_ntk.foreach_po(
        [&topo_ntk, &old2new, &required_outputs](const auto& po)
        {
            const auto po_node = topo_ntk.get_node(po);
            if (!topo_ntk.is_constant(po_node))
            {
                const auto output_pin =
                    static_cast<std::size_t>(topo_ntk.is_multioutput(po_node) ? signal_output_pin<Ntk>(po) : 0u);
                ensure_output_pin_capacity(old2new, required_outputs, topo_ntk, po_node,
                                           static_cast<uint32_t>(output_pin));
                required_outputs[po_node][output_pin] = true;
            }
        });

    topo_ntk.foreach_pi(
        [&topo_ntk, &klut_ntk, &old2new, &required_outputs](const auto& pi)
        {
            ensure_output_pin_capacity(old2new, required_outputs, topo_ntk, pi);

            const auto pi_sig = klut_ntk.create_pi();
            for (auto& mapped_output : old2new[pi])
            {
                mapped_output = pi_sig;
            }
        });

    topo_ntk.foreach_gate(
        [&topo_ntk, &klut_ntk, &old2new, &required_outputs](const auto& gate)
        {
            ensure_output_pin_capacity(old2new, required_outputs, topo_ntk, gate);

            std::vector<klut_signal> children{};
            children.reserve(topo_ntk.fanin_size(gate));

            topo_ntk.foreach_fanin(gate,
                                   [&topo_ntk, &klut_ntk, &old2new, &required_outputs, &children](const auto& fanin)
                                   {
                                       const auto fanin_node = topo_ntk.get_node(fanin);
                                       auto       child      = klut_ntk.get_constant(false);

                                       if (topo_ntk.is_constant(fanin_node))
                                       {
                                           child = klut_ntk.get_constant(topo_ntk.constant_value(fanin_node));
                                       }
                                       else
                                       {
                                           const auto output_pin = static_cast<std::size_t>(
                                               topo_ntk.is_multioutput(fanin_node) ? signal_output_pin<Ntk>(fanin) :
                                                                                     0u);
                                           ensure_output_pin_capacity(old2new, required_outputs, topo_ntk, fanin_node,
                                                                      static_cast<uint32_t>(output_pin));
                                           child = old2new[fanin_node][output_pin];
                                       }

                                       if (topo_ntk.is_complemented(fanin))
                                       {
                                           child = klut_ntk.create_not(child);
                                       }

                                       children.push_back(child);
                                   });

            if (topo_ntk.is_multioutput(gate))
            {
                const auto output_pin_count = node_output_pin_count(topo_ntk, gate);

                for (uint32_t pin = 0u; pin < output_pin_count; ++pin)
                {
                    if (required_outputs[gate][pin])
                    {
                        old2new[gate][pin] = klut_ntk.create_node(children, topo_ntk.node_function_pin(gate, pin));
                    }
                }

                const auto first_required_pin =
                    std::find(required_outputs[gate].cbegin(), required_outputs[gate].cend(), true);
                if (first_required_pin != required_outputs[gate].cend())
                {
                    const auto representative_pin =
                        static_cast<uint32_t>(std::distance(required_outputs[gate].cbegin(), first_required_pin));

                    for (uint32_t pin = 0u; pin < output_pin_count; ++pin)
                    {
                        if (!required_outputs[gate][pin])
                        {
                            old2new[gate][pin] = old2new[gate][representative_pin];
                        }
                    }
                }
            }
            else
            {
                const auto gate_sig = klut_ntk.create_node(children, topo_ntk.node_function(gate));
                for (auto& mapped_output : old2new[gate])
                {
                    mapped_output = gate_sig;
                }
            }
        });

    topo_ntk.foreach_po(
        [&topo_ntk, &klut_ntk, &old2new](const auto& po)
        {
            const auto po_node = topo_ntk.get_node(po);
            auto       po_sig  = klut_ntk.get_constant(false);

            if (topo_ntk.is_constant(po_node))
            {
                po_sig = klut_ntk.get_constant(topo_ntk.constant_value(po_node));
            }
            else
            {
                const auto output_pin =
                    static_cast<std::size_t>(topo_ntk.is_multioutput(po_node) ? signal_output_pin<Ntk>(po) : 0u);
                po_sig = old2new[po_node][output_pin];
            }

            if (topo_ntk.is_complemented(po))
            {
                po_sig = klut_ntk.create_not(po_sig);
            }

            klut_ntk.create_po(po_sig);
        });

    return klut_ntk;
}

/**
 * @brief Prepares a network for SAT-based equivalence checking.
 *
 * Multi-output networks are transformed into single-output kLUT networks while preserving output-pin semantics.
 *
 * @tparam NtkOrLyt Source network or layout type.
 * @param ntk_or_lyt Source network or layout.
 * @return Equivalent `klut_network` used for miter construction.
 */
template <typename NtkOrLyt>
mockturtle::klut_network prepare_for_equivalence_checking(const NtkOrLyt& ntk_or_lyt)
{
    if constexpr (mockturtle::has_is_multioutput_v<NtkOrLyt> && mockturtle::has_node_function_pin_v<NtkOrLyt>)
    {
        return split_multioutput_network(ntk_or_lyt);
    }

    return mockturtle::cleanup_dangling<NtkOrLyt, mockturtle::klut_network>(ntk_or_lyt, true, false);
}

template <typename Spec, typename Impl>
class equivalence_checking_impl
{
  public:
    /**
     * Standard constructor.
     *
     * @param specification Logical specification of intended functionality.
     * @param implementation Implementation of specified functionality.
     * @param st Statistics.
     */
    explicit equivalence_checking_impl(const Spec& specification, const Impl& implementation,
                                       equivalence_checking_stats& st) :
            spec{specification},
            impl{implementation},
            pst{st}
    {}

    eq_type run() noexcept
    {
        mockturtle::stopwatch stop{pst.runtime};

        if constexpr (is_gate_level_layout_v<Spec>)
        {
            if (has_drvs(spec, &pst.spec_drv_stats))
            {
                return eq_type::NO;
            }
        }
        if constexpr (is_gate_level_layout_v<Impl>)
        {
            if (has_drvs(impl, &pst.impl_drv_stats))
            {
                return eq_type::NO;
            }
        }

        const auto spec_ntk = prepare_for_equivalence_checking(spec);
        const auto impl_ntk = prepare_for_equivalence_checking(impl);
        const auto miter    = mockturtle::miter<mockturtle::klut_network>(spec_ntk, impl_ntk);

        if (miter)
        {
            mockturtle::equivalence_checking_stats st;

            const auto eq = mockturtle::equivalence_checking(*miter, {}, &st);

            if (eq.has_value())
            {
                pst.eq = *eq ? eq_type::STRONG : eq_type::NO;

                if (pst.eq == eq_type::STRONG)
                {
                    // compute TP of specification
                    if constexpr (fiction::is_gate_level_layout_v<Spec>)
                    {
                        const auto cp_tp = fiction::critical_path_length_and_throughput(spec);

                        pst.tp_spec = static_cast<int64_t>(cp_tp.throughput);
                    }
                    // compute TP of implementation
                    if constexpr (fiction::is_gate_level_layout_v<Impl>)
                    {
                        const auto cp_tp = fiction::critical_path_length_and_throughput(impl);

                        pst.tp_impl = static_cast<int64_t>(cp_tp.throughput);
                    }

                    pst.tp_diff = std::abs(pst.tp_spec - pst.tp_impl);

                    if (pst.tp_diff != 0)
                    {
                        pst.eq = eq_type::WEAK;
                    }
                }

                if (!(*eq))
                {
                    pst.counter_example = st.counter_example;
                }
            }
            else
            {
                std::cout << "[e] resource limit exceeded" << std::endl;

                return eq_type::NO;
            }
        }
        else
        {
            std::cout << "[w] both networks/layouts must have the same number of primary inputs and outputs"
                      << std::endl;

            return eq_type::NO;
        }

        return pst.eq;
    }

  private:
    /**
     * Specification.
     */
    const Spec spec;
    /**
     * Implementation.
     */
    const Impl impl;

    equivalence_checking_stats& pst;

    template <typename NtkOrLyt>
    bool has_drvs(const NtkOrLyt& ntk_or_lyt, gate_level_drv_stats* stats) const noexcept
    {
        fiction::gate_level_drv_params drv_ps{};

        // suppress DRV output
        std::ostringstream null_stream{};
        drv_ps.out = &null_stream;

        gate_level_drvs(ntk_or_lyt, drv_ps, stats);

        return stats->drvs != 0;
    }
};

}  // namespace detail

/**
 * Performs SAT-based equivalence checking between a specification of type `Spec` and an implementation of type `Impl`.
 * Both `Spec` and `Impl` need to be network types (that is, gate-level layouts can be utilized as well).
 *
 * This implementation enables the comparison of two logic networks, a logic network and a gate-level layout or two
 * gate-level layouts. Since gate-level layouts have a notion of timing that logic networks do not, this function does
 * not simply prove logical equivalence but, additionally, takes timing aspects into account as well.
 *
 * Thereby, three different types of equivalences arise:
 *
 * - `NO` equivalence: Spec and Impl are not logically equivalent or one of them is a gate-level layout that contains
 * DRVs and, thus, cannot be checked for equivalence.
 * - `WEAK` equivalence: Spec and Impl are logically equivalent but either one of them is a gate-level layout with TP of
 * \f$\frac{1}{x}\f$ with \f$x > 1\f$ or both of them are gate-level layouts with TP of \f$\frac{1}{x}\f$ and
 * \f$\frac{1}{y}\f$, respectively, where \f$x \neq y\f$.
 * - `STRONG` equivalence: Spec and Impl are logically equivalent and all involved gate-level layouts have TP of
 * \f$\frac{1}{1}\f$.
 *
 * This approach was first proposed in \"Verification for Field-coupled Nanocomputing Circuits\" by M. Walter, R. Wille,
 * F. Sill Torres, D. Große, and R. Drechsler in DAC 2020.
 *
 * @tparam Spec Specification type.
 * @tparam Impl Implementation type.
 * @param spec The specification.
 * @param impl The implementation.
 * @param pst Statistics.
 * @return The equivalence type of `spec` and `impl`.
 */
template <typename Spec, typename Impl>
eq_type equivalence_checking(const Spec& spec, const Impl& impl, equivalence_checking_stats* pst = nullptr)
{
    static_assert(mockturtle::is_network_type_v<Spec>, "Spec is not a network type");
    static_assert(mockturtle::is_network_type_v<Impl>, "Impl is not a network type");

    equivalence_checking_stats        st{};
    detail::equivalence_checking_impl p{spec, impl, st};

    const auto result = p.run();

    if (pst)
    {
        *pst = st;
    }

    return result;
}

}  // namespace fiction

#endif  // FICTION_EQUIVALENCE_CHECKING_HPP
