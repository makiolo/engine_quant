#include "quant-cpp/include/quant_cpp_bridge.hpp"

#include "quant-cpp/include/legacy_irs_leaf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace quant::legacy {

namespace {

double bond(double a, double b, double sigma, double r0, double t, double maturity) noexcept {
    const double tau = maturity - t;
    if (!(a > 0.0) || !std::isfinite(a) || !std::isfinite(b) || !std::isfinite(sigma) ||
        !std::isfinite(r0) || !std::isfinite(tau) || tau < 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double b_factor = -std::expm1(-a * tau) / a;
    const double a2 = a * a;
    const double sigma2 = sigma * sigma;
    const double term1 = (b_factor - tau) * (a2 * b - 0.5 * sigma2) / a2;
    const double term2 = sigma2 * b_factor * b_factor / (4.0 * a);
    return std::exp(term1 - term2 - b_factor * r0);
}

}  // namespace

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
    double* output) noexcept {
    if (times == 0 || payment_times == nullptr || accruals == nullptr || output == nullptr ||
        !std::isfinite(notional) || !std::isfinite(fixed_rate) || !std::isfinite(start)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double p_start = bond(a, b, sigma, r0, 0.0, start);
    const double p_end = bond(a, b, sigma, r0, 0.0, payment_times[times - 1]);
    double denominator = 0.0;
    double fixed_discounted = 0.0;
    double previous = start;
    for (std::size_t i = 0; i < times; ++i) {
        const double ti = payment_times[i];
        const double tau = accruals[i];
        if (!std::isfinite(ti) || !std::isfinite(tau) || ti <= previous || tau <= 0.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double discount = bond(a, b, sigma, r0, 0.0, ti);
        if (!std::isfinite(discount)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        denominator += tau * discount;
        fixed_discounted += tau * discount;
        previous = ti;
    }
    double rate = fixed_rate;
    if (use_par_rate) {
        const double par_end = bond(a, b, sigma, r0, start, payment_times[times - 1]);
        double par_denominator = 0.0;
        for (std::size_t i = 0; i < times; ++i) {
            const double discount = bond(a, b, sigma, r0, start, payment_times[i]);
            par_denominator += accruals[i] * discount;
        }
        rate = (1.0 - par_end) / par_denominator;
    }
    const double value = notional * ((p_start - p_end) - rate * fixed_discounted);
    if (!std::isfinite(value)) return std::numeric_limits<double>::quiet_NaN();
    *output = value;
    return value;
}

}  // namespace quant::legacy

namespace quant::bridge {

namespace {
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kIrsHullWhite = 1;
constexpr std::uint64_t kAnalytic = 1ULL << 0;
constexpr std::uint64_t kCpu = 1ULL << 4;

CppStatus status(CppErrorCode code, const char* message) {
    return CppStatus{code, rust::String(message)};
}

CppStatus status(CppErrorCode code, const std::string& message) {
    return CppStatus{code, rust::String(message)};
}

bool finite_slice(rust::Slice<const double> values) noexcept {
    for (const double value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

}  // namespace

std::uint32_t bridge_version() noexcept { return kVersion; }

std::uint64_t CppKernel::capabilities() const noexcept {
    return kind_ == kIrsHullWhite ? (kAnalytic | kCpu) : 0;
}

std::unique_ptr<CppKernel> new_kernel(std::uint32_t kind, rust::Slice<const std::uint8_t> config) {
    if (kind != kIrsHullWhite) throw std::invalid_argument("unsupported C++ kernel kind");
    if (config.size() < 8) throw std::invalid_argument("quant-cpp config is truncated");
    const std::uint32_t version = static_cast<std::uint32_t>(config[0]) |
        (static_cast<std::uint32_t>(config[1]) << 8) |
        (static_cast<std::uint32_t>(config[2]) << 16) |
        (static_cast<std::uint32_t>(config[3]) << 24);
    if (version != kVersion) throw std::invalid_argument("unsupported quant-cpp bridge version");
    const std::uint32_t configured_kind = static_cast<std::uint32_t>(config[4]) |
        (static_cast<std::uint32_t>(config[5]) << 8) |
        (static_cast<std::uint32_t>(config[6]) << 16) |
        (static_cast<std::uint32_t>(config[7]) << 24);
    if (configured_kind != kind) throw std::invalid_argument("quant-cpp config kind mismatch");
    return std::unique_ptr<CppKernel>(new CppKernel(kind));
}

CppStatus CppKernel::price_batch(
    BatchShape shape,
    rust::Slice<const double> market,
    rust::Slice<const double> model,
    rust::Slice<const double> products,
    rust::Slice<const double> payment_times,
    rust::Slice<const double> accruals,
    rust::Slice<double> output) const {
    try {
        if (kind_ != kIrsHullWhite) return status(CppErrorCode::Unsupported, "unknown kernel kind");
        if (shape.scenarios != 1 || shape.factors != 1) {
            return status(CppErrorCode::Unsupported, "IRS leaf supports one scenario and one factor");
        }
        if (shape.times != payment_times.size() || payment_times.size() != accruals.size()) {
            return status(CppErrorCode::InvalidArgument, "schedule shape does not match BatchShape");
        }
        if (shape.trades > std::numeric_limits<std::size_t>::max() / 4 ||
            products.size() != static_cast<std::size_t>(shape.trades) * 4 ||
            output.size() != static_cast<std::size_t>(shape.trades)) {
            return status(CppErrorCode::InvalidArgument, "product/output shape mismatch");
        }
        if (shape.trades == 0) return status(CppErrorCode::Ok, "");
        if (market.size() != 1 || model.size() != 3 || shape.times == 0) {
            return status(CppErrorCode::InvalidArgument, "market/model/schedule shape mismatch");
        }
        if (!finite_slice(market) || !finite_slice(model) || !finite_slice(products) ||
            !finite_slice(payment_times) || !finite_slice(accruals)) {
            return status(CppErrorCode::InvalidArgument, "inputs must be finite");
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(shape.trades); ++i) {
            const double* product = products.data() + i * 4;
            const double value = quant::legacy::irs_hull_white_npv(
                model[0], model[1], model[2], market[0], product[0], product[1], product[3] != 0.0,
                product[2], payment_times.data(), accruals.data(), payment_times.size(), output.data() + i);
            if (!std::isfinite(value)) {
                return status(CppErrorCode::NumericalFailure, "legacy IRS kernel returned a non-finite value");
            }
        }
        return status(CppErrorCode::Ok, "");
    } catch (const std::bad_alloc&) {
        return status(CppErrorCode::ResourceExhausted, "C++ allocation failed");
    } catch (const std::exception& error) {
        return status(CppErrorCode::Internal, error.what());
    } catch (...) {
        return status(CppErrorCode::Internal, "unknown C++ exception");
    }
}

}  // namespace quant::bridge
