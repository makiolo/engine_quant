#include "engine/payoff/market_path.hpp"

#include <utility>

namespace engine {
namespace payoff {

void MarketPath::set_fixing(ObservableId observable, TimePoint time, double value) {
    fixings_[observable].push_back(FixingEntry{time, value});
}

bool MarketPath::has_fixing(const ObservableId& observable, TimePoint time) const {
    auto it = fixings_.find(observable);
    if (it == fixings_.end()) return false;
    for (const auto& entry : it->second) {
        if (time_equal(entry.time, time)) return true;
    }
    return false;
}

double MarketPath::fixing(const ObservableId& observable, TimePoint time, const NodePath& path) const {
    auto it = fixings_.find(observable);
    if (it != fixings_.end()) {
        for (const auto& entry : it->second) {
            if (time_equal(entry.time, time)) return entry.value;
        }
    }
    throw EvaluationError("fixing ausente para observable '" + observable.value + "'", path);
}

std::vector<double> MarketPath::fixings_up_to(const ObservableId& observable, TimePoint cutoff) const {
    std::vector<double> values;
    auto it = fixings_.find(observable);
    if (it == fixings_.end()) return values;
    for (const auto& entry : it->second) {
        if (!time_less(cutoff, entry.time)) values.push_back(entry.value);
    }
    return values;
}

void MarketPath::set_discount_factor(CurveId curve, TimePoint from, TimePoint to, double value) {
    discount_factors_[curve].push_back(DiscountEntry{from, to, value});
}

double MarketPath::discount_factor(const CurveId& curve, TimePoint from, TimePoint to, const NodePath& path) const {
    auto it = discount_factors_.find(curve);
    if (it != discount_factors_.end()) {
        for (const auto& entry : it->second) {
            if (time_equal(entry.from, from) && time_equal(entry.to, to)) return entry.value;
        }
    }
    throw EvaluationError("discount factor ausente para curva '" + curve.value + "'", path);
}

void MarketPath::set_fx_rate(Currency from_currency, Currency to_currency, TimePoint time, double value) {
    fx_rates_.push_back(FxEntry{std::move(from_currency), std::move(to_currency), time, value});
}

double MarketPath::fx_rate(
    const Currency& from_currency, const Currency& to_currency, TimePoint time, const NodePath& path
) const {
    for (const auto& entry : fx_rates_) {
        if (entry.from_currency == from_currency && entry.to_currency == to_currency && time_equal(entry.time, time)) {
            return entry.value;
        }
    }
    throw EvaluationError(
        "tipo de cambio ausente para '" + from_currency.code + "->" + to_currency.code + "'", path
    );
}

void FixingStore::set(ObservableId observable, TimePoint time, double value) {
    values_[observable].push_back(Entry{time, value});
}

bool FixingStore::has(const ObservableId& observable, TimePoint time) const {
    auto it = values_.find(observable);
    if (it == values_.end()) return false;
    for (const auto& entry : it->second) {
        if (time_equal(entry.time, time)) return true;
    }
    return false;
}

double FixingStore::get(const ObservableId& observable, TimePoint time, const NodePath& path) const {
    auto it = values_.find(observable);
    if (it != values_.end()) {
        for (const auto& entry : it->second) {
            if (time_equal(entry.time, time)) return entry.value;
        }
    }
    throw EvaluationError("fixing historico ausente para observable '" + observable.value + "'", path);
}

} // namespace payoff
} // namespace engine
