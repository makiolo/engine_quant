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

// PLAN.md §7.19: conversion helpers para el lote de ExposureProfile -- un
// rust::Vec<ffi::ExposureProfileResult> por trade en cada sentido, mismo patron elemento a
// elemento que ya usa irs_hull_white_exposure_profile para un unico perfil.
rust::Vec<ffi::ExposureProfileResult> to_rust_profiles(const std::vector<ExposureProfile>& profiles) {
    rust::Vec<ffi::ExposureProfileResult> out;
    out.reserve(profiles.size());
    for (const auto& p : profiles) {
        out.push_back(ffi::ExposureProfileResult{to_rust_vec(p.times), to_rust_vec(p.ee), to_rust_vec(p.pfe_95)});
    }
    return out;
}

std::vector<ExposureProfile> from_rust_profiles(const rust::Vec<ffi::ExposureProfileResult>& profiles) {
    std::vector<ExposureProfile> out;
    out.reserve(profiles.size());
    for (const auto& p : profiles) {
        out.push_back(ExposureProfile{
            std::vector<double>(p.times.begin(), p.times.end()),
            std::vector<double>(p.ee.begin(), p.ee.end()),
            std::vector<double>(p.pfe_95.begin(), p.pfe_95.end())
        });
    }
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

double hull_white_2f_zero_coupon_bond(double a, double b, double sigma, double eta, double rho, double r0, double maturity) {
    return ffi::hull_white_2f_zero_coupon_bond(a, b, sigma, eta, rho, r0, maturity);
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

std::vector<double> irs_hull_white_npv_batch(
    double a, double b, double sigma, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    rust::Vec<double> result = ffi::irs_hull_white_npv_batch(
        a, b, sigma, r0, to_rust_vec(notionals), to_rust_vec(fixed_rates), start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
    return std::vector<double>(result.begin(), result.end());
}

std::vector<double> irs_hull_white_npv_delta_r0_batch(
    double a, double b, double sigma, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    rust::Vec<double> result = ffi::irs_hull_white_npv_delta_r0_batch(
        a, b, sigma, r0, to_rust_vec(notionals), to_rust_vec(fixed_rates), start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
    return std::vector<double>(result.begin(), result.end());
}

std::vector<ExposureProfile> irs_hull_white_exposure_profile_batch(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
) {
    rust::Vec<ffi::ExposureProfileResult> result = ffi::irs_hull_white_exposure_profile_batch(
        backend,
        a, b, sigma, r0,
        to_rust_vec(notionals), to_rust_vec(fixed_rates), start,
        to_rust_vec(payment_times),
        to_rust_vec(accruals),
        to_rust_vec(monitoring_times),
        n_steps, n_paths, seed
    );
    return from_rust_profiles(result);
}

std::vector<double> unilateral_cva_from_exposure_batch(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    const std::vector<ExposureProfile>& profiles,
    double hazard_rate, double recovery_rate
) {
    rust::Vec<double> result = ffi::unilateral_cva_from_exposure_batch(
        backend, a, b, sigma, r0, to_rust_profiles(profiles), hazard_rate, recovery_rate
    );
    return std::vector<double>(result.begin(), result.end());
}

ExposureProfile irs_hull_white_2f_exposure_profile(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate,
    double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
) {
    ffi::ExposureProfileResult result = ffi::irs_hull_white_2f_exposure_profile(
        backend,
        a, b, sigma, eta, rho, r0,
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

double unilateral_cva_from_exposure_2f(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
) {
    return ffi::unilateral_cva_from_exposure_2f(
        backend,
        a, b, sigma, eta, rho, r0,
        to_rust_vec(times), to_rust_vec(ee),
        hazard_rate, recovery_rate
    );
}

double irs_hull_white_2f_npv(
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    return ffi::irs_hull_white_2f_npv(
        a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
}

double irs_hull_white_2f_npv_delta_r0(
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    return ffi::irs_hull_white_2f_npv_delta_r0(
        a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
}

std::vector<double> irs_hull_white_2f_npv_batch(
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    rust::Vec<double> result = ffi::irs_hull_white_2f_npv_batch(
        a, b, sigma, eta, rho, r0, to_rust_vec(notionals), to_rust_vec(fixed_rates), start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
    return std::vector<double>(result.begin(), result.end());
}

std::vector<double> irs_hull_white_2f_npv_delta_r0_batch(
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    rust::Vec<double> result = ffi::irs_hull_white_2f_npv_delta_r0_batch(
        a, b, sigma, eta, rho, r0, to_rust_vec(notionals), to_rust_vec(fixed_rates), start,
        to_rust_vec(payment_times), to_rust_vec(accruals)
    );
    return std::vector<double>(result.begin(), result.end());
}

std::vector<ExposureProfile> irs_hull_white_2f_exposure_profile_batch(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
) {
    rust::Vec<ffi::ExposureProfileResult> result = ffi::irs_hull_white_2f_exposure_profile_batch(
        backend,
        a, b, sigma, eta, rho, r0,
        to_rust_vec(notionals), to_rust_vec(fixed_rates), start,
        to_rust_vec(payment_times),
        to_rust_vec(accruals),
        to_rust_vec(monitoring_times),
        n_steps, n_paths, seed
    );
    return from_rust_profiles(result);
}

std::vector<double> unilateral_cva_from_exposure_2f_batch(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<ExposureProfile>& profiles,
    double hazard_rate, double recovery_rate
) {
    rust::Vec<double> result = ffi::unilateral_cva_from_exposure_2f_batch(
        backend, a, b, sigma, eta, rho, r0, to_rust_profiles(profiles), hazard_rate, recovery_rate
    );
    return std::vector<double>(result.begin(), result.end());
}

} // namespace engine
