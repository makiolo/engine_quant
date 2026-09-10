#include "engine/market.hpp"

#include "engine/engine.hpp" // hull_white_zero_coupon_bond

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace engine {

MarketSnapshot::MarketSnapshot(std::vector<double> pillars, std::vector<double> zero_rates)
    : pillars_(std::move(pillars)), zero_rates_(std::move(zero_rates)) {
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
    double a, double b, double sigma, double r0, const std::vector<double>& pillars
) {
    std::vector<double> zero_rates;
    zero_rates.reserve(pillars.size());
    for (double t : pillars) {
        double price = hull_white_zero_coupon_bond(a, b, sigma, r0, 0.0, t);
        zero_rates.push_back(t > 0.0 ? -std::log(price) / t : 0.0);
    }
    return MarketSnapshot(pillars, std::move(zero_rates));
}

} // namespace engine
