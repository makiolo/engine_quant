#include "engine/market.hpp"

#include "engine/engine.hpp" // hull_white_zero_coupon_bond

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace engine {

MarketSnapshot::MarketSnapshot(
    std::vector<double> pillars, std::vector<double> zero_rates, double hazard_rate, double recovery_rate
)
    : pillars_(std::move(pillars)), zero_rates_(std::move(zero_rates)),
      hazard_rate_(hazard_rate), recovery_rate_(recovery_rate) {
    if (pillars_.size() != zero_rates_.size()) {
        throw std::invalid_argument("MarketSnapshot: pillars y zero_rates deben tener el mismo tamano");
    }
    if (pillars_.empty()) {
        throw std::invalid_argument("MarketSnapshot: necesita al menos un pillar");
    }
    for (std::size_t i = 1; i < pillars_.size(); ++i) {
        if (pillars_[i] <= pillars_[i - 1]) {
            throw std::invalid_argument("MarketSnapshot: pillars debe ser estrictamente creciente");
        }
    }
}

MarketSnapshot::MarketSnapshot(const Params& params)
    : MarketSnapshot(
          get_vector(params, "pillars"), get_vector(params, "zero_rates"),
          get_double(params, "hazard_rate", 0.0), get_double(params, "recovery_rate", 0.0)
      ) {}

double MarketSnapshot::zero_rate(double t) const {
    if (t <= pillars_.front()) return zero_rates_.front();
    if (t >= pillars_.back()) return zero_rates_.back();

    auto it = std::lower_bound(pillars_.begin(), pillars_.end(), t);
    std::size_t i = static_cast<std::size_t>(it - pillars_.begin());
    if (i == 0) i = 1; // t == pillars_.front() ya cubierto arriba, pero por robustez
    double t0 = pillars_[i - 1], t1 = pillars_[i];
    double z0 = zero_rates_[i - 1], z1 = zero_rates_[i];
    double w = (t - t0) / (t1 - t0);
    return z0 + w * (z1 - z0);
}

double MarketSnapshot::discount_factor(double t) const {
    return std::exp(-zero_rate(t) * t);
}

MarketSnapshot MarketSnapshot::synthetic_from_hull_white(
    double a, double b, double sigma, double r0, const std::vector<double>& pillars,
    double hazard_rate, double recovery_rate
) {
    std::vector<double> zero_rates;
    zero_rates.reserve(pillars.size());
    for (double t : pillars) {
        double price = hull_white_zero_coupon_bond(a, b, sigma, r0, 0.0, t);
        zero_rates.push_back(t > 0.0 ? -std::log(price) / t : 0.0);
    }
    return MarketSnapshot(pillars, std::move(zero_rates), hazard_rate, recovery_rate);
}

MarketSnapshot MarketSnapshot::synthetic_from_hull_white_2f(
    double a, double b, double sigma, double eta, double rho, double r0, const std::vector<double>& pillars,
    double hazard_rate, double recovery_rate
) {
    std::vector<double> zero_rates;
    zero_rates.reserve(pillars.size());
    for (double t : pillars) {
        double price = hull_white_2f_zero_coupon_bond(a, b, sigma, eta, rho, r0, t);
        zero_rates.push_back(t > 0.0 ? -std::log(price) / t : 0.0);
    }
    return MarketSnapshot(pillars, std::move(zero_rates), hazard_rate, recovery_rate);
}

} // namespace engine
