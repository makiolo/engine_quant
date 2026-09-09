#include "engine/engine.hpp"

#include "engine-ffi-cxx/lib.h"

namespace engine {

double ping() {
    return ffi::ping();
}

double hull_white_zero_coupon_bond(double a, double b, double sigma, double r0, double t, double maturity) {
    return ffi::hull_white_zero_coupon_bond(a, b, sigma, r0, t, maturity);
}

double hull_white_zero_coupon_bond_delta_r0(double a, double b, double sigma, double r0, double t, double maturity) {
    return ffi::hull_white_zero_coupon_bond_delta_r0(a, b, sigma, r0, t, maturity);
}

double irs_unilateral_cva_5y(
    double a,
    double b,
    double sigma,
    double r0,
    double notional,
    double hazard_rate,
    double recovery_rate,
    std::uint64_t n_paths,
    std::uint64_t seed
) {
    return ffi::irs_unilateral_cva_5y(a, b, sigma, r0, notional, hazard_rate, recovery_rate, n_paths, seed);
}

} // namespace engine
