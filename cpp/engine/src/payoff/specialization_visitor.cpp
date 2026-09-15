#include "engine/payoff/specialization_visitor.hpp"

#include <cmath>
#include <memory>

#include "engine/payoff/expression.hpp"

namespace engine {
namespace payoff {
namespace specialization {

namespace {

struct WhenCashflow {
    TimePoint time;
    Currency currency;
    double amount;
};

// Reconoce `When(t, Cashflow(ccy, Constant(amount)))`: la única forma de cashflow que
// `irs_swap` produce (irs_templates.cpp) -- un `amount` que no sea un `Constant` desnudo (p.ej.
// una expresión con `DiscountFactor`/`Fixing`) no es el patrón que esta especialización
// reconoce.
std::optional<WhenCashflow> extract_when_cashflow(const ContractPtr& node) {
    auto when_node = std::dynamic_pointer_cast<const When>(node);
    if (!when_node) return std::nullopt;
    auto cf = std::dynamic_pointer_cast<const Cashflow>(when_node->child());
    if (!cf) return std::nullopt;
    auto amount_const = std::dynamic_pointer_cast<const Constant>(cf->amount());
    if (!amount_const) return std::nullopt;
    return WhenCashflow{when_node->time(), cf->currency(), amount_const->value()};
}

} // namespace

std::optional<RecognizedIrsSwap> recognize_irs_swap(const ContractPtr& root) {
    auto outer = std::dynamic_pointer_cast<const Both>(root);
    if (!outer || outer->children().size() != 2) return std::nullopt;

    auto floating_leg = std::dynamic_pointer_cast<const Both>(outer->children()[0]);
    auto give_fixed = std::dynamic_pointer_cast<const Give>(outer->children()[1]);
    if (!floating_leg || !give_fixed) return std::nullopt;
    if (floating_leg->children().size() != 2) return std::nullopt;

    auto fixed_leg = std::dynamic_pointer_cast<const Both>(give_fixed->child());
    if (!fixed_leg) return std::nullopt;

    std::optional<WhenCashflow> start_cf = extract_when_cashflow(floating_leg->children()[0]);
    std::optional<WhenCashflow> end_cf = extract_when_cashflow(floating_leg->children()[1]);
    if (!start_cf || !end_cf) return std::nullopt;
    if (start_cf->currency != end_cf->currency) return std::nullopt;
    if (std::abs(start_cf->amount + end_cf->amount) > 1e-9) return std::nullopt;

    RecognizedIrsSwap result;
    result.currency = start_cf->currency;
    result.notional = start_cf->amount;
    result.start = start_cf->time;
    result.end = end_cf->time;

    result.fixed_payments.reserve(fixed_leg->children().size());
    for (const ContractPtr& child : fixed_leg->children()) {
        std::optional<WhenCashflow> cf = extract_when_cashflow(child);
        if (!cf || cf->currency != result.currency) return std::nullopt;
        result.fixed_payments.emplace_back(cf->time, cf->amount);
    }
    return result;
}

double specialized_present_value(const RecognizedIrsSwap& irs, const MarketSnapshot& market) {
    double floating_leg = irs.notional * (market.discount_factor(irs.start.year_fraction) - market.discount_factor(irs.end.year_fraction));

    double fixed_leg = 0.0;
    for (const std::pair<TimePoint, double>& payment : irs.fixed_payments) {
        fixed_leg += payment.second * market.discount_factor(payment.first.year_fraction);
    }
    return floating_leg - fixed_leg;
}

double specialized_dv01(const RecognizedIrsSwap& irs, const MarketSnapshot& market, double bump) {
    double base = specialized_present_value(irs, market);
    double bumped = specialized_present_value(irs, bump_market_parallel(market, bump));
    return bumped - base;
}

} // namespace specialization
} // namespace payoff
} // namespace engine
