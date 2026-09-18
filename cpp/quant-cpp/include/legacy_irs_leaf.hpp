#pragma once

#include <cstddef>

namespace quant::legacy {

// Leaf extracted from the validated engine Hull-White/Vasicek IRS PV formula. It deliberately
// uses pointer/length views so the C++ adapter never owns or retains Rust input buffers.
double irs_hull_white_npv(
    double a,
    double b,
    double sigma,
    double r0,
    double notional,
    double fixed_rate,
    bool use_par_rate,
    double start,
    const double* payment_times,
    const double* accruals,
    std::size_t times,
    double* output) noexcept;

}  // namespace quant::legacy
