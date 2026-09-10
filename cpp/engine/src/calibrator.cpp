#include "engine/calibrator.hpp"

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

CalibrationResult HullWhite1FCalibrator::calibrate(const MarketSnapshot& market, const Params& initial_guess) const {
    double initial_a = get_double(initial_guess, "a");
    double initial_b = get_double(initial_guess, "b");
    double sigma = get_double(initial_guess, "sigma");
    double r0 = get_double(initial_guess, "r0");

    ffi::HullWhiteCalibrationResult raw = ffi::calibrate_hull_white(
        to_rust_vec(market.pillars()), to_rust_vec(market.zero_rates()), initial_a, initial_b, sigma, r0
    );

    CalibrationResult result;
    result.optimal_params = Params{
        {"a", raw.a},
        {"b", raw.b},
        {"sigma", raw.sigma},
        {"r0", raw.r0},
    };
    result.rmse = raw.rmse;
    result.iterations = static_cast<int>(raw.iterations);
    result.converged = raw.converged;
    return result;
}

CalibrationResult HullWhite2FCalibrator::calibrate(const MarketSnapshot& market, const Params& initial_guess) const {
    double initial_a = get_double(initial_guess, "a");
    double initial_b = get_double(initial_guess, "b");
    double sigma = get_double(initial_guess, "sigma");
    double eta = get_double(initial_guess, "eta");
    double rho = get_double(initial_guess, "rho");
    double r0 = get_double(initial_guess, "r0");

    ffi::HullWhite2FCalibrationResult raw = ffi::calibrate_hull_white_2f(
        to_rust_vec(market.pillars()), to_rust_vec(market.zero_rates()), initial_a, initial_b, sigma, eta, rho, r0
    );

    CalibrationResult result;
    result.optimal_params = Params{
        {"a", raw.a},
        {"b", raw.b},
        {"sigma", raw.sigma},
        {"eta", raw.eta},
        {"rho", raw.rho},
        {"r0", raw.r0},
    };
    result.rmse = raw.rmse;
    result.iterations = static_cast<int>(raw.iterations);
    result.converged = raw.converged;
    return result;
}

} // namespace engine
