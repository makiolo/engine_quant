#include "engine/payoff/measures.hpp"

#include <stdexcept>
#include <utility>

#include "engine/payoff/errors.hpp"
#include "engine/payoff/event_state.hpp"
#include "engine/payoff/scenario_evaluator.hpp"

namespace engine {
namespace payoff {

DiscountingPolicy::DiscountingPolicy(Currency currency, CurveId curve) {
    set_curve(std::move(currency), std::move(curve));
}

void DiscountingPolicy::set_curve(Currency currency, CurveId curve) {
    curve_by_currency_[std::move(currency)] = std::move(curve);
}

const CurveId& DiscountingPolicy::curve_for(const Currency& currency) const {
    auto it = curve_by_currency_.find(currency);
    if (it == curve_by_currency_.end()) {
        throw std::invalid_argument("DiscountingPolicy: sin curva de descuento registrada para moneda '" + currency.code + "'");
    }
    return it->second;
}

CashflowLedger evaluate_cashflows(const ContractPtr& root, EvaluationContext& context) {
    ScenarioEvaluator evaluator;
    return evaluator.evaluate(root, context);
}

double discount_and_convert(
    const CashflowLedger& ledger, const MarketPath& path, const DiscountingPolicy& discounting,
    const Currency& reporting_currency, TimePoint valuation_time
) {
    double total = 0.0;
    for (std::size_t i = 0; i < ledger.size(); ++i) {
        const LedgerEntry& entry = ledger[i];
        NodePath diag = NodePath::root().child(i, "cashflow");
        double df = path.discount_factor(discounting.curve_for(entry.currency), valuation_time, entry.payment_time, diag);
        double amount = entry.amount * df;
        if (entry.currency != reporting_currency) {
            amount *= path.fx_rate(entry.currency, reporting_currency, entry.payment_time, diag);
        }
        total += amount;
    }
    return total;
}

ValuationResult scenario_payoff(
    const ContractPtr& root, EvaluationContext& context, const DiscountingPolicy& discounting,
    Currency reporting_currency, TimePoint valuation_time
) {
    ValuationResult result;
    result.ledger = evaluate_cashflows(root, context);
    result.present_value = discount_and_convert(result.ledger, context.path, discounting, reporting_currency, valuation_time);
    result.reporting_currency = std::move(reporting_currency);
    result.measure = ProbabilityMeasure::DeterministicScenario;
    result.dependencies = DependencyVisitor{}.analyze(root);
    return result;
}

ValuationResult present_value(
    const ContractPtr& root, EvaluationContext& context, const DiscountingPolicy& discounting,
    Currency reporting_currency, TimePoint valuation_time
) {
    return scenario_payoff(root, context, discounting, std::move(reporting_currency), valuation_time);
}

double bump_and_reval_curve(
    const ContractPtr& root, const MarketPath& base_path, const FixingStore& historical_fixings,
    const DiscountingPolicy& discounting, const Currency& reporting_currency, const CurveId& curve,
    double zero_rate_bump, TimePoint valuation_time
) {
    RuntimeState base_state;
    EvaluationContext base_context{base_path, historical_fixings, base_state};
    ValuationResult base_result = present_value(root, base_context, discounting, reporting_currency, valuation_time);

    MarketPath bumped_path = base_path.with_curve_bump(curve, zero_rate_bump);
    RuntimeState bumped_state;
    EvaluationContext bumped_context{bumped_path, historical_fixings, bumped_state};
    ValuationResult bumped_result = present_value(root, bumped_context, discounting, reporting_currency, valuation_time);

    return bumped_result.present_value - base_result.present_value;
}

double bump_and_reval_fixing(
    const ContractPtr& root, const MarketPath& base_path, const FixingStore& historical_fixings,
    const DiscountingPolicy& discounting, const Currency& reporting_currency,
    const ObservableId& observable, double bump, TimePoint valuation_time
) {
    RuntimeState base_state;
    EvaluationContext base_context{base_path, historical_fixings, base_state};
    ValuationResult base_result = present_value(root, base_context, discounting, reporting_currency, valuation_time);

    MarketPath bumped_path = base_path.with_fixing_bump(observable, bump);
    RuntimeState bumped_state;
    EvaluationContext bumped_context{bumped_path, historical_fixings, bumped_state};
    ValuationResult bumped_result = present_value(root, bumped_context, discounting, reporting_currency, valuation_time);

    return bumped_result.present_value - base_result.present_value;
}

} // namespace payoff
} // namespace engine
