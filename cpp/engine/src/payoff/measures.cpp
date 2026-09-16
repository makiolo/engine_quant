#include "engine/payoff/measures.hpp"

#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

#include "engine-ffi-cxx/lib.h"
#include "engine/payoff/canonical_visitor.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/event_state.hpp"
#include "engine/payoff/model_capabilities.hpp"
#include "engine/payoff/scenario_evaluator.hpp"

namespace engine {
namespace payoff {

namespace {

// Mismo patron que engine.cpp/calibrator.cpp (duplicado por traduccion unit, no hay helper
// compartido hoy): cxx no convierte implicitamente std::vector<double> <-> rust::Vec<double>.
rust::Vec<double> to_rust_vec(const std::vector<double>& v) {
    rust::Vec<double> out;
    out.reserve(v.size());
    for (double x : v) out.push_back(x);
    return out;
}

rust::Vec<rust::String> to_rust_string_vec(const std::vector<std::string>& v) {
    rust::Vec<rust::String> out;
    out.reserve(v.size());
    for (const std::string& s : v) out.push_back(rust::String(s));
    return out;
}

// Preflight comun a las medidas GBM de este archivo, tanto bajo Q (Fase 5/6) como bajo P
// (Fase 7): capacidades declaradas, `required_measure` soportada, todo observable referenciado
// generado por el modelo, y -- si el contrato usa `Monitoring::ContinuousApproximation` -- el
// modelo declara `supports_continuous_barrier_bridge`. Nunca simula una sola ruta antes de pasar
// este chequeo. Generalizado sobre `required_measure` (en vez de fijar `RiskNeutralQ`) para que
// `GbmModel`/`GbmPModel` compartan el mismo chequeo: `GbmModel::capabilities()` solo declara
// `RiskNeutralQ` y `GbmPModel::capabilities()` solo `PhysicalP`, asi que pedir la medida
// equivocada a cualquiera de los dos falla aqui -- "el motor rechaza combinaciones Q/P
// invalidas", criterio de aceptacion explicito de Fase 7.
void preflight_gbm_capabilities(
    const PayoffProgram& program, const std::optional<ModelCapabilities>& capabilities,
    ProbabilityMeasure required_measure
) {
    if (!capabilities.has_value()) {
        throw ValidationError("el modelo no declara ModelCapabilities", NodePath::root());
    }
    if (capabilities->supported_measures.find(required_measure) == capabilities->supported_measures.end()) {
        throw ValidationError(
            "el modelo no soporta la medida requerida (" +
                std::string(required_measure == ProbabilityMeasure::RiskNeutralQ ? "RiskNeutralQ" : "PhysicalP") +
                ")",
            NodePath::root()
        );
    }

    DependencyReport dependencies = DependencyVisitor{}.analyze(program.contract);
    for (const ObservableId& observable : dependencies.observables) {
        if (capabilities->generated_observables.find(observable) == capabilities->generated_observables.end()) {
            throw ValidationError(
                "observable '" + observable.value + "' no generado por el modelo (preflight de Fase 5)",
                NodePath::root()
            );
        }
    }
    if (dependencies.requires_continuous_barrier_bridge && !capabilities->supports_continuous_barrier_bridge) {
        throw ValidationError(
            "el contrato usa Monitoring::ContinuousApproximation (Brownian bridge) pero el modelo no lo "
            "soporta (ModelCapabilities::supports_continuous_barrier_bridge=false, PLAN_PRODUCTS.md §4.2 "
            "Fase 6)",
            NodePath::root()
        );
    }
    if (dependencies.requires_early_exercise_regression && !capabilities->supports_early_exercise_regression) {
        throw ValidationError(
            "el contrato usa Exercise (Longstaff-Schwartz) pero el modelo no lo soporta "
            "(ModelCapabilities::supports_early_exercise_regression=false, PLAN_PRODUCTS.md §10 Fase 9)",
            NodePath::root()
        );
    }
}

} // namespace

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
    const PayoffProgram& program, const GbmModel& model, std::uint64_t n_paths, std::uint64_t seed,
    double valuation_time
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffQPriceResult result = ffi::price_payoff_gbm_q(
            spec_json, model.observable().value, model.s0(), model.r(), model.q(), model.sigma(), n_paths, seed,
            valuation_time
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

ExercisePolicyResult exercise_price_gbm(
    const PayoffProgram& program, const GbmModel& model, std::uint64_t n_paths, std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::ExercisePolicyResult result = ffi::price_payoff_exercise_gbm_q(
            spec_json, model.observable().value, model.s0(), model.r(), model.q(), model.sigma(), n_paths, seed
        );
        ExercisePolicyResult out;
        out.price.mean = result.mean;
        out.price.std_error = result.std_error;
        out.price.ci_low = result.ci_low;
        out.price.ci_high = result.ci_high;
        out.price.n_paths = result.n_paths;
        out.price.measure = ProbabilityMeasure::RiskNeutralQ;
        out.dates.reserve(result.dates.size());
        for (const ffi::ExerciseDateDiagnosticResult& d : result.dates) {
            ExerciseDateDiagnostic diag;
            diag.date = d.date;
            diag.n_in_the_money = d.n_in_the_money;
            diag.has_regression = d.has_regression;
            diag.coeff_a = d.coeff_a;
            diag.coeff_b = d.coeff_b;
            diag.coeff_c = d.coeff_c;
            diag.exercised_fraction = d.exercised_fraction;
            out.dates.push_back(diag);
        }
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

HitProbabilityResult hit_probability_gbm(
    const PayoffProgram& program, const GbmModel& model, const EventId& event, std::uint64_t n_paths,
    std::uint64_t seed
) {
    // Que 'event' no exista en el contrato es un error de EVALUACION (lo detecta la compilacion
    // del JSON en Rust, que conoce los EventId declarados), no de preflight de capacidades.
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);

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

ExposureProfile payoff_exposure_profile_gbm(
    const PayoffProgram& program, const GbmModel& model, const std::vector<TimePoint>& exposure_times,
    std::uint64_t n_paths, std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);

    std::vector<double> exposure_times_raw;
    exposure_times_raw.reserve(exposure_times.size());
    for (TimePoint t : exposure_times) exposure_times_raw.push_back(t.year_fraction);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::ExposureProfileResult result = ffi::payoff_exposure_profile_gbm_q(
            spec_json, model.observable().value, model.s0(), model.r(), model.q(), model.sigma(),
            to_rust_vec(exposure_times_raw), n_paths, seed
        );
        ExposureProfile out;
        out.times = std::vector<double>(result.times.begin(), result.times.end());
        out.ee = std::vector<double>(result.ee.begin(), result.ee.end());
        out.pfe_95 = std::vector<double>(result.pfe_95.begin(), result.pfe_95.end());
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

ForecastResult forecast_gbm_p(
    const PayoffProgram& program, const GbmPModel& model, std::uint64_t n_paths, std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::PhysicalP);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffPForecastResult result =
            ffi::forecast_gbm_p(spec_json, model.observable().value, model.s0(), model.mu(), model.sigma(), n_paths, seed);
        ForecastResult out;
        out.mean = result.mean;
        out.std_error = result.std_error;
        out.ci_low = result.ci_low;
        out.ci_high = result.ci_high;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::PhysicalP;
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

HitProbabilityResult hit_probability_gbm(
    const PayoffProgram& program, const GbmPModel& model, const EventId& event, std::uint64_t n_paths,
    std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::PhysicalP);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffPHitProbabilityResult result = ffi::hit_probability_gbm_p(
            spec_json, event.value, model.observable().value, model.s0(), model.mu(), model.sigma(), n_paths, seed
        );
        HitProbabilityResult out;
        out.probability = result.probability;
        out.std_error = result.std_error;
        out.ci_low = result.ci_low;
        out.ci_high = result.ci_high;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::PhysicalP;
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

PnlDistributionResult pnl_distribution_gbm_p(
    const PayoffProgram& program, const GbmPModel& model, std::uint64_t n_paths, std::uint64_t seed,
    double confidence
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::PhysicalP);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PnlDistributionResult result = ffi::pnl_distribution_gbm_p(
            spec_json, model.observable().value, model.s0(), model.mu(), model.sigma(), n_paths, seed, confidence
        );
        PnlDistributionResult out;
        out.mean = result.mean;
        out.std_error = result.std_error;
        out.var = result.var;
        out.es = result.es;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::PhysicalP;
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

// --- Fase 11 (items pendientes del cierre documentado en PLAN_PRODUCTS.md §12) ---------------

SensitivityResult payoff_sensitivity_gbm(
    const PayoffProgram& program, const GbmModel& model, const std::string& greek, std::uint64_t n_paths,
    std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffSensitivityResult result = ffi::payoff_sensitivity_gbm_q(
            spec_json, model.observable().value, greek, model.s0(), model.r(), model.q(), model.sigma(), n_paths,
            seed
        );
        SensitivityResult out;
        out.value = result.value;
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

SensitivityResult payoff_sensitivity_gbm_p(
    const PayoffProgram& program, const GbmPModel& model, const std::string& greek, std::uint64_t n_paths,
    std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::PhysicalP);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffSensitivityResult result = ffi::payoff_sensitivity_gbm_p(
            spec_json, model.observable().value, greek, model.s0(), model.mu(), model.sigma(), n_paths, seed
        );
        SensitivityResult out;
        out.value = result.value;
        out.std_error = result.std_error;
        out.ci_low = result.ci_low;
        out.ci_high = result.ci_high;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::PhysicalP;
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

bool payoff_contains_exercise(const PayoffProgram& program) {
    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        return ffi::payoff_contains_exercise(spec_json);
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

// --- PLAN_HYPERDUAL.md §5 (revisado): Gamma/Vanna via likelihood ratio -----------------------

SensitivityResult payoff_sensitivity2_gbm(
    const PayoffProgram& program, const GbmModel& model, const std::string& greek, std::uint64_t n_paths,
    std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffSensitivityResult result = ffi::payoff_sensitivity2_gbm_q(
            spec_json, model.observable().value, greek, model.s0(), model.r(), model.q(), model.sigma(), n_paths,
            seed
        );
        SensitivityResult out;
        out.value = result.value;
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

SensitivityResult payoff_sensitivity2_gbm_p(
    const PayoffProgram& program, const GbmPModel& model, const std::string& greek, std::uint64_t n_paths,
    std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::PhysicalP);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffSensitivityResult result = ffi::payoff_sensitivity2_gbm_p(
            spec_json, model.observable().value, greek, model.s0(), model.mu(), model.sigma(), n_paths, seed
        );
        SensitivityResult out;
        out.value = result.value;
        out.std_error = result.std_error;
        out.ci_low = result.ci_low;
        out.ci_high = result.ci_high;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::PhysicalP;
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

SensitivityResult payoff_sensitivity_cross_gbm(
    const PayoffProgram& program, const GbmModel& model, const std::string& risk_factor,
    const std::string& cross_factor, std::uint64_t n_paths, std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffSensitivityResult result = ffi::payoff_sensitivity_cross_gbm_q(
            spec_json, model.observable().value, risk_factor, cross_factor, model.s0(), model.r(), model.q(),
            model.sigma(), n_paths, seed
        );
        SensitivityResult out;
        out.value = result.value;
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

SensitivityResult payoff_sensitivity_cross_gbm_p(
    const PayoffProgram& program, const GbmPModel& model, const std::string& risk_factor,
    const std::string& cross_factor, std::uint64_t n_paths, std::uint64_t seed
) {
    preflight_gbm_capabilities(program, model.capabilities(), ProbabilityMeasure::PhysicalP);

    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        ffi::PayoffSensitivityResult result = ffi::payoff_sensitivity_cross_gbm_p(
            spec_json, model.observable().value, risk_factor, cross_factor, model.s0(), model.mu(), model.sigma(),
            n_paths, seed
        );
        SensitivityResult out;
        out.value = result.value;
        out.std_error = result.std_error;
        out.ci_low = result.ci_low;
        out.ci_high = result.ci_high;
        out.n_paths = result.n_paths;
        out.measure = ProbabilityMeasure::PhysicalP;
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

bool payoff_supports_second_order_lrm(const PayoffProgram& program) {
    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        return ffi::payoff_supports_second_order_lrm(spec_json);
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

bool payoff_supports_second_order_lrm_p(const PayoffProgram& program) {
    std::string spec_json = CanonicalVisitor::to_json(program.id, program.contract);
    try {
        return ffi::payoff_supports_second_order_lrm_p(spec_json);
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

HedgeResult synthesize_hedge_gbm(
    const PayoffProgram& target, const std::vector<const PayoffProgram*>& instruments, const GbmModel& model,
    const std::optional<std::vector<double>>& instrument_prices, double ridge, const HedgeConstraints& constraints,
    bool compute_residual_greeks, std::uint64_t n_paths, std::uint64_t seed
) {
    preflight_gbm_capabilities(target, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);
    for (const PayoffProgram* instrument : instruments) {
        preflight_gbm_capabilities(*instrument, model.capabilities(), ProbabilityMeasure::RiskNeutralQ);
    }

    std::string target_spec_json = CanonicalVisitor::to_json(target.id, target.contract);
    std::vector<std::string> instrument_specs_json;
    instrument_specs_json.reserve(instruments.size());
    for (const PayoffProgram* instrument : instruments) {
        instrument_specs_json.push_back(CanonicalVisitor::to_json(instrument->id, instrument->contract));
    }

    rust::Vec<double> prices_rust = instrument_prices.has_value() ? to_rust_vec(*instrument_prices) : rust::Vec<double>{};

    rust::Vec<double> lower_bounds;
    rust::Vec<double> upper_bounds;
    if (!constraints.bounds.empty()) {
        lower_bounds.reserve(constraints.bounds.size());
        upper_bounds.reserve(constraints.bounds.size());
        for (const auto& [lo, hi] : constraints.bounds) {
            lower_bounds.push_back(lo);
            upper_bounds.push_back(hi);
        }
    }
    double max_gross_notional = constraints.max_gross_notional.value_or(-1.0);

    try {
        ffi::HedgeSynthesisResult result = ffi::synthesize_hedge_gbm_q(
            target_spec_json, to_rust_string_vec(instrument_specs_json), model.observable().value, model.s0(),
            model.r(), model.q(), model.sigma(), prices_rust, ridge, lower_bounds, upper_bounds, max_gross_notional,
            compute_residual_greeks, n_paths, seed
        );

        HedgeResult out;
        out.weights = std::vector<double>(result.weights.begin(), result.weights.end());
        out.residuals = std::vector<double>(result.residuals.begin(), result.residuals.end());
        out.residual_mean = result.residual_mean;
        out.residual_std = result.residual_std;
        out.residual_max_abs = result.residual_max_abs;
        if (result.has_cost) out.cost = result.cost;
        if (result.has_gross_notional) out.gross_notional = result.gross_notional;
        if (result.has_residual_greeks) {
            HedgeResidualGreeks greeks;
            greeks.delta = result.residual_greeks_delta;
            greeks.rho = result.residual_greeks_rho;
            greeks.dividend_yield = result.residual_greeks_dividend_yield;
            greeks.vega = result.residual_greeks_vega;
            out.residual_greeks = greeks;
        }
        return out;
    } catch (const std::exception& e) {
        throw EvaluationError(e.what(), NodePath::root());
    }
}

} // namespace payoff
} // namespace engine
