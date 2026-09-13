#include "engine/payoff/measures.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "engine-ffi-cxx/lib.h"
#include "engine/payoff/canonical_visitor.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/event_state.hpp"
#include "engine/payoff/model_capabilities.hpp"
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

QValuationResult risk_neutral_price_gbm(
    const PayoffProgram& program, const GbmModel& model, std::uint64_t n_paths, std::uint64_t seed
) {
    std::optional<ModelCapabilities> capabilities = model.capabilities();
    if (!capabilities.has_value()) {
        throw ValidationError("GbmModel no declara ModelCapabilities", NodePath::root());
    }
    if (capabilities->supported_measures.find(ProbabilityMeasure::RiskNeutralQ) == capabilities->supported_measures.end()) {
        throw ValidationError("el modelo no soporta la medida RiskNeutralQ", NodePath::root());
    }

    // Preflight (PLAN_PRODUCTS.md §12 Fase 5, criterio de aceptacion): ningun observable del
    // contrato entra en el bridge hacia Rust sin que el modelo declare que lo genera.
    DependencyReport dependencies = DependencyVisitor{}.analyze(program.contract);
    for (const ObservableId& observable : dependencies.observables) {
        if (capabilities->generated_observables.find(observable) == capabilities->generated_observables.end()) {
            throw ValidationError(
                "observable '" + observable.value + "' no generado por el modelo (preflight de Fase 5)",
                NodePath::root()
            );
        }
    }

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffQPriceResult result = ffi::price_payoff_gbm_q(
            spec_json, model.observable().value, model.s0(), model.r(), model.q(), model.sigma(), n_paths, seed
        );
        QValuationResult out;
        out.mean = result.mean;
        out.std_error = result.std_error;
        out.ci_low = result.ci_low;
        out.ci_high = result.ci_high;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::RiskNeutralQ;
        return out;
    } catch (const std::exception& e) {
        // Un Err(String) del lado Rust (compilacion del JSON, p.ej. un nodo no soportado en
        // CompiledPayoff v1) cruza como excepcion de C++ (ver rust/crates/engine-ffi) -- se
        // relanza en el vocabulario de errores del motor de payoff en vez de dejar escapar el
        // tipo de excepcion interno de cxx.
        throw EvaluationError(e.what(), NodePath::root());
    }
}

HitProbabilityResult hit_probability_gbm(
    const PayoffProgram& program, const GbmModel& model, const EventId& event, std::uint64_t n_paths,
    std::uint64_t seed
) {
    std::optional<ModelCapabilities> capabilities = model.capabilities();
    if (!capabilities.has_value()) {
        throw ValidationError("GbmModel no declara ModelCapabilities", NodePath::root());
    }
    if (capabilities->supported_measures.find(ProbabilityMeasure::RiskNeutralQ) == capabilities->supported_measures.end()) {
        throw ValidationError("el modelo no soporta la medida RiskNeutralQ", NodePath::root());
    }

    // Mismo preflight de observables que risk_neutral_price_gbm (PLAN_PRODUCTS.md §12 Fase 5/6):
    // que event no exista en el contrato es un error de EVALUACION (lo detecta la compilacion
    // del JSON en Rust, que conoce los EventId declarados), no de preflight de observables.
    DependencyReport dependencies = DependencyVisitor{}.analyze(program.contract);
    for (const ObservableId& observable : dependencies.observables) {
        if (capabilities->generated_observables.find(observable) == capabilities->generated_observables.end()) {
            throw ValidationError(
                "observable '" + observable.value + "' no generado por el modelo (preflight de Fase 5)",
                NodePath::root()
            );
        }
    }

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffQHitProbabilityResult result = ffi::hit_probability_gbm_q(
            spec_json, event.value, model.observable().value, model.s0(), model.r(), model.q(), model.sigma(),
            n_paths, seed
        );
        HitProbabilityResult out;
        out.probability = result.probability;
        out.std_error = result.std_error;
        out.ci_low = result.ci_low;
        out.ci_high = result.ci_high;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::RiskNeutralQ;
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

} // namespace payoff
} // namespace engine
