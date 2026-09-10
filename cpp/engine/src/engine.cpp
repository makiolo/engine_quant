#include "engine/engine.hpp"

#include "engine-ffi-cxx/lib.h"

namespace engine {

namespace {

rust::Vec<double> to_rust_vec(const std::vector<double>& v) {
    rust::Vec<double> out;
    out.reserve(v.size());
    for (double x : v) out.push_back(x);
    return out;
}

} // namespace

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

ExposureProfile irs_hull_white_exposure_profile(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate,
    double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
) {
    ffi::ExposureProfileResult result = ffi::irs_hull_white_exposure_profile(
        backend,
        a, b, sigma, r0,
        notional, fixed_rate, use_par_rate,
        start,
        to_rust_vec(payment_times),
        to_rust_vec(accruals),
        to_rust_vec(monitoring_times),
        n_steps, n_paths, seed
    );

    return ExposureProfile{
        std::vector<double>(result.times.begin(), result.times.end()),
        std::vector<double>(result.ee.begin(), result.ee.end()),
        std::vector<double>(result.pfe_95.begin(), result.pfe_95.end())
    };
}

double unilateral_cva_from_exposure(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
) {
    return ffi::unilateral_cva_from_exposure(
        backend,
        a, b, sigma, r0,
        to_rust_vec(times), to_rust_vec(ee),
        hazard_rate, recovery_rate
    );
}

bool is_gpu_backend_available() {
    return ffi::is_gpu_backend_available();
}

double irs_hull_white_npv(
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    return ffi::irs_hull_white_npv(
        a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
}

double irs_hull_white_npv_delta_r0(
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    return ffi::irs_hull_white_npv_delta_r0(
        a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
}

} // namespace engine
