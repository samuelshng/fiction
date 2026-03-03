//
// Created by marcel on 26.05.21.
//

#ifndef FICTION_EQUIVALENCE_CHECKING_UTILS_HPP
#define FICTION_EQUIVALENCE_CHECKING_UTILS_HPP

#include <catch2/catch_test_macros.hpp>

#include <fiction/algorithms/verification/equivalence_checking.hpp>

template <typename Spec, typename Impl>
void check_eq(const Spec& spec, const Impl& impl)
{
    fiction::equivalence_checking_stats st{};

    const auto eq = fiction::equivalence_checking(spec, impl, &st);

    UNSCOPED_INFO("equivalence_result=" << static_cast<int>(eq));
    UNSCOPED_INFO("spec_drvs=" << st.spec_drv_stats.drvs << ", impl_drvs=" << st.impl_drv_stats.drvs);

    if (st.spec_drv_stats.drvs != 0u)
    {
        UNSCOPED_INFO("spec_drv_report=\n" << st.spec_drv_stats.report.dump(2));
    }

    if (st.impl_drv_stats.drvs != 0u)
    {
        UNSCOPED_INFO("impl_drv_report=\n" << st.impl_drv_stats.report.dump(2));
    }

    CHECK(eq != fiction::eq_type::NO);
}

#endif  // FICTION_EQUIVALENCE_CHECKING_UTILS_HPP
