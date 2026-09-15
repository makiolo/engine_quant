#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/engine.hpp" // ExposureProfile, ver payoff_exposure_profile_gbm (Fase 6)
#include "engine/model.hpp"
#include "engine/payoff/contract.hpp"
#include "engine/payoff/dependency_visitor.hpp"
#include "engine/payoff/evaluation_context.hpp"
#include "engine/payoff/ledger.hpp"
#include "engine/payoff/market_path.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/payoff/probability_measure.hpp"

namespace engine {
namespace payoff {

// Que curva descuenta cada moneda del ledger (§5.2 `ValuationContext::discounting`). Ausencia de
// una moneda pedida es error explicito (`curve_for`), nunca una curva por defecto silenciosa
// (§3.1).
class DiscountingPolicy {
public:
    DiscountingPolicy() = default;
    // Atajo mono-moneda/mono-curva (el caso mas comun: call/forward de un unico activo, o el
    // swap del test cruzado con legacy -- una unica moneda de pago).
    DiscountingPolicy(Currency currency, CurveId curve);

    void set_curve(Currency currency, CurveId curve);
    const CurveId& curve_for(const Currency& currency) const;

private:
    std::unordered_map<Currency, CurveId> curve_by_currency_;
};

// Resultado de valorar un `PayoffProgram` bajo una unica ruta determinista (§6, §14): ledger
// pathwise sin agregar, PV agregado en `reporting_currency`, dependencias del contrato y la
// medida probabilistica bajo la que se obtuvo -- Fase 4 siempre `DeterministicScenario` (una
// ruta conocida, ni Q ni P: §6 "ScenarioPayoff usa una ruta determinista y no finge ser ni Q ni
// P").
struct ValuationResult {
    CashflowLedger ledger;
    double present_value = 0.0;
    Currency reporting_currency;
    ProbabilityMeasure measure = ProbabilityMeasure::DeterministicScenario;
    DependencyReport dependencies;
};

// "Cashflows" (§12 Fase 4): ejecuta el contrato sobre una ruta conocida y devuelve el ledger
// pathwise intacto, sin descontar ni convertir (§3.5 "el visitor de auditoria puede devolverlo
// intacto"). Delgado sobre `ScenarioEvaluator`; existe como funcion nombrada porque Fase 4 la
// trata como medida de primera clase junto a `scenario_payoff`/`present_value`, no como detalle
// interno de estas.
CashflowLedger evaluate_cashflows(const ContractPtr& root, EvaluationContext& context);

// Descuenta y convierte un ledger ya calculado a `reporting_currency` en `valuation_time` (§5.2,
// §12 Fase 4 "descuento/conversion multi-moneda explicitos"): por cada entrada,
// `discount_factor(discounting.curve_for(entry.currency), valuation_time, entry.payment_time)`
// y, si la moneda de la entrada difiere de `reporting_currency`,
// `fx_rate(entry.currency, reporting_currency, entry.payment_time)`. La misma moneda que
// `reporting_currency` nunca busca un fx_rate registrado -- es la conversion identidad, no la
// conversion "implicita" entre monedas distintas que prohibe §3.2.
double discount_and_convert(
    const CashflowLedger& ledger, const MarketPath& path, const DiscountingPolicy& discounting,
    const Currency& reporting_currency, TimePoint valuation_time = TimePoint{0.0}
);

// "ScenarioPayoff" (§12 Fase 4, §6): ejecuta el contrato sobre una unica ruta conocida y
// descuenta/convierte el ledger resultante a `reporting_currency`. Nunca finge ser Q ni P (§6).
ValuationResult scenario_payoff(
    const ContractPtr& root, EvaluationContext& context, const DiscountingPolicy& discounting,
    Currency reporting_currency, TimePoint valuation_time = TimePoint{0.0}
);

// "PV por ledger" (§12 Fase 4): identico a `scenario_payoff` en Fase 4 -- documentado como
// funcion separada porque "PV" es el nombre que expondra `Registry<IMeasure>` (Fase 8+) y porque
// dejara de ser un alias cuando Fase 5+ promedie muchas rutas Monte Carlo bajo Q en vez de
// evaluar una unica ruta determinista. El resultado ya informa
// `ProbabilityMeasure::DeterministicScenario` (criterio de aceptacion de Fase 4).
ValuationResult present_value(
    const ContractPtr& root, EvaluationContext& context, const DiscountingPolicy& discounting,
    Currency reporting_currency, TimePoint valuation_time = TimePoint{0.0}
);

// Bump-and-reval generico (§12 Fase 4 "bump-and-reval generico por observable/curva", mismo
// estilo que `Dv01Measure`/`compute_dv01` en measure.cpp): repricia el mismo contrato bajo una
// copia de `base_path` con la curva/observable indicado desplazado y devuelve
// `PV(bumped) - PV(base)`. Cada llamada usa su propio `RuntimeState` interno -- una ruta nunca
// comparte estado con otra (§2) -- el llamante no necesita pasarlo.
//
// `bump_and_reval_curve`: desplazamiento PARALELO de tipo cero de `zero_rate_bump` sobre
// `curve` (ver `MarketPath::with_curve_bump`).
double bump_and_reval_curve(
    const ContractPtr& root, const MarketPath& base_path, const FixingStore& historical_fixings,
    const DiscountingPolicy& discounting, const Currency& reporting_currency, const CurveId& curve,
    double zero_rate_bump, TimePoint valuation_time = TimePoint{0.0}
);

// `bump_and_reval_fixing`: desplazamiento PARALELO aditivo de `bump` sobre todos los fixings
// registrados de `observable` en `base_path` (Delta de un observable de spot/subyacente, ver
// `MarketPath::with_fixing_bump`).
double bump_and_reval_fixing(
    const ContractPtr& root, const MarketPath& base_path, const FixingStore& historical_fixings,
    const DiscountingPolicy& discounting, const Currency& reporting_currency,
    const ObservableId& observable, double bump, TimePoint valuation_time = TimePoint{0.0}
);

// Resultado de precio bajo Q via Monte Carlo (PLAN_PRODUCTS.md §12 Fase 5): media, error
// estandar e intervalo de confianza del estimador (espejo de `engine_core::mc::McEstimate` en
// Rust), mas la medida para trazabilidad (§14). A diferencia de `ValuationResult` (una unica
// ruta determinista), no hay `ledger`: el ledger pathwise vive por ruta Monte Carlo dentro de
// Rust y no cruza la frontera, solo el agregado.
struct QValuationResult {
    double mean = 0.0;
    double std_error = 0.0;
    double ci_low = 0.0;
    double ci_high = 0.0;
    std::uint64_t n_paths = 0;
    ProbabilityMeasure measure = ProbabilityMeasure::RiskNeutralQ;
};

// Precio bajo Q (Monte Carlo, GBM) de un `PayoffProgram` (PLAN_PRODUCTS.md §12 Fase 5).
// Preflight ANTES de simular una sola ruta (criterio de aceptacion explicito de esta fase):
// compara `DependencyVisitor::analyze(program.contract)` contra `model.capabilities()` y lanza
// `ValidationError` si el contrato referencia un observable que el modelo no genera o pide una
// medida que no soporta. Delegado sobre `engine::ffi::price_payoff_gbm_q`
// (`rust/crates/engine-ffi`), que a su vez compila/evalua el `CompiledPayoff` enteramente en
// Rust (`engine_core::payoff`) -- un error de compilacion del JSON (p.ej. un nodo no soportado
// en Fase 5, ver `docs/schema/engine.payoff/v1.schema.json`) llega como excepcion de Rust y se
// relanza aqui como `EvaluationError`.
//
// `valuation_time` (PLAN_GREEKS.md §7.2/Fase 5, aditivo, default `0.0`): desplaza "hoy" hacia
// adelante manteniendo `model`/`market` intactos -- Theta puro (`engine::greeks::compute_greek`,
// `RiskFactorKind::TimeShift`) es la unica llamante que pasa un valor distinto de `0.0`, via
// `PricingContext::pricing_date()`. Un `valuation_time` que alcanza o supera un instante
// requerido por el contrato lanza `EvaluationError` explicito (este motor no modela un
// `FixingStore` para la ruta Monte Carlo, PLAN_GREEKS.md §7.4) -- nunca aproxima en silencio.
QValuationResult risk_neutral_price_gbm(
    const PayoffProgram& program, const GbmModel& model, std::uint64_t n_paths, std::uint64_t seed,
    double valuation_time = 0.0
);

// Resultado de HitProbability bajo Q (PLAN_PRODUCTS.md §12 Fase 6: "hit probability Q como
// medida separada del PV"). Misma forma que `QValuationResult` (espejo de
// `engine_core::mc::McEstimate` en Rust) pero `probability` es una probabilidad en `[0,1]`, no un
// valor monetario -- sin `ledger` por el mismo motivo que `QValuationResult` no lo tiene.
struct HitProbabilityResult {
    double probability = 0.0;
    double std_error = 0.0;
    double ci_low = 0.0;
    double ci_high = 0.0;
    std::uint64_t n_paths = 0;
    ProbabilityMeasure measure = ProbabilityMeasure::RiskNeutralQ;
};

// Diagnostico de UNA fecha de decision de un `Exercise`, resuelto por Longstaff-Schwartz
// (PLAN_PRODUCTS.md §10, Fase 9: "diagnostico de regresion y politica de ejercicio exportable").
// `has_regression=false` cuando en `date` no hubo suficientes rutas in-the-money para ajustar la
// regresion cuadratica -- en ese caso `coeff_a/b/c` son `0.0` (no un ajuste real: `has_regression`
// es la unica forma de distinguirlo de un ajuste legitimo de coeficientes nulos) y
// `exercised_fraction` es `0.0` (nadie ejercito ese dia, por falta de evidencia, nunca por
// extrapolacion -- ver `engine_core::payoff::lsm::resolve_exercise_decisions` en Rust).
struct ExerciseDateDiagnostic {
    double date = 0.0;
    std::uint64_t n_in_the_money = 0;
    bool has_regression = false;
    double coeff_a = 0.0;
    double coeff_b = 0.0;
    double coeff_c = 0.0;
    double exercised_fraction = 0.0;
};

// Precio bajo Q de un `PayoffProgram` con un derecho de ejercicio (PLAN_PRODUCTS.md §10, Fase 9):
// mismo precio Monte Carlo que `QValuationResult` (`price`) mas la politica de ejercicio
// exportable (`dates`, en el mismo orden ascendente que las fechas declaradas en el contrato) --
// criterio de aceptacion explicito de esta fase ("explain muestra la politica").
struct ExercisePolicyResult {
    QValuationResult price;
    std::vector<ExerciseDateDiagnostic> dates;
};

// Precio bajo Q (Monte Carlo, GBM, Longstaff-Schwartz) de un `PayoffProgram` con exactamente un
// derecho de ejercicio (`Exercise`, PLAN_PRODUCTS.md §10, Fase 9). Mismo preflight que
// `risk_neutral_price_gbm` (dependencias vs. capacidades del modelo) mas
// `ModelCapabilities::supports_early_exercise_regression` (ver `DependencyReport::
// requires_early_exercise_regression`, comparado antes de simular una sola ruta, igual que
// `requires_continuous_barrier_bridge` en Fase 6). El contrato debe contener EXACTAMENTE un
// `Exercise` -- ver el doc-comment de `engine_core::payoff::lsm` (Rust) para el porque de esa
// restriccion en esta fase; 0 o >1 llega como `EvaluationError` desde la compilacion del JSON en
// Rust, mismo mecanismo que cualquier otro error de `CompiledPayoff`.
//
// Determinista dado `seed`: ninguna decision de ejercicio se toma por ruta de forma aislada --
// se resuelve una unica vez por Longstaff-Schwartz sobre TODO el lote de rutas simuladas con ese
// `seed` (ver `engine_core::payoff::lsm::resolve_exercise_decisions`), sin aleatoriedad propia
// adicional -- "decisiones reproducibles con seed, sin look-ahead", criterio de aceptacion
// explicito de esta fase.
ExercisePolicyResult exercise_price_gbm(
    const PayoffProgram& program, const GbmModel& model, std::uint64_t n_paths, std::uint64_t seed
);

// Probabilidad bajo Q (Monte Carlo, GBM) de que `event` -- un `Trigger` de `program.contract`,
// identificado por su `EventId` -- dispare en la ruta (PLAN_PRODUCTS.md §12 Fase 6). Mismo
// preflight que `risk_neutral_price_gbm` (capacidades del modelo vs. dependencias del contrato);
// si `event` no nombra ningun `Trigger` del arbol, el error llega desde Rust (compilacion del
// JSON conoce los `EventId` declarados) y se relanza como `EvaluationError`, igual que un nodo no
// soportado.
HitProbabilityResult hit_probability_gbm(
    const PayoffProgram& program, const GbmModel& model, const EventId& event, std::uint64_t n_paths,
    std::uint64_t seed
);

// Perfil de exposicion PATHWISE bajo Q de un `PayoffProgram` (PLAN_PRODUCTS.md §12 Fase 6:
// "perfil de exposicion pathwise a partir del mismo AST y netting explicito"). Reutiliza el
// mismo `ExposureProfile` (times/ee/pfe_95) que la ruta legacy IRS+Hull-White
// (`engine::ExposureProfile` en engine.hpp) -- sin inventar un tipo de resultado nuevo (§5.5).
//
// Deliberadamente NO es una valoracion condicional/anidada: `EE(t)`/`PFE95(t)` se calculan sobre
// el valor REALIZADO restante de cada ruta ya simulada (los cashflows del ledger de esa ruta con
// `payment_time >= t`, descontados desde `t`), no `E_Q[V_t | F_t]` via regresion -- eso es
// Longstaff-Schwartz (Fase 9), y solo para `Exercise`. Ver el doc-comment de
// `engine_core::payoff::payoff_exposure_profile_gbm_q` (Rust) para el detalle. Si `program`
// combina varios trades con `Both`, el netting es automatico (el ledger ya los combina en la
// misma ruta), sin paso aparte.
ExposureProfile payoff_exposure_profile_gbm(
    const PayoffProgram& program, const GbmModel& model, const std::vector<TimePoint>& exposure_times,
    std::uint64_t n_paths, std::uint64_t seed
);

// Resultado de "Forecast" bajo P (PLAN_PRODUCTS.md §12 Fase 7). Misma forma que `QValuationResult`
// (media/error estandar/intervalo de confianza del estimador Monte Carlo) pero `measure =
// PhysicalP` y, a diferencia de esa medida, NUNCA descontado: bajo P no existe un numerario libre
// de riesgo canonico (§6 "P se reserva para forecast ... con drift/calibracion fisicos
// explicitos") -- `mean` es la suma esperada de cashflows tal cual el ledger la produce, en la
// fecha de pago de cada uno. Struct separado de `QValuationResult` (aunque tenga la misma forma)
// para que el nombre no sugiera un precio Q ni una cantidad descontada.
struct ForecastResult {
    double mean = 0.0;
    double std_error = 0.0;
    double ci_low = 0.0;
    double ci_high = 0.0;
    std::uint64_t n_paths = 0;
    ProbabilityMeasure measure = ProbabilityMeasure::PhysicalP;
};

// "Forecast" bajo P (Monte Carlo, GBM fisico) de un `PayoffProgram` (PLAN_PRODUCTS.md §12
// Fase 7). Preflight ANTES de simular: `model.capabilities()` debe declarar soporte de
// `PhysicalP` (nunca `RiskNeutralQ` -- un `GbmModel` de Fase 5 se rechaza aqui, igual que un
// `GbmPModel` se rechaza en `risk_neutral_price_gbm`, satisfaciendo "el motor rechaza
// combinaciones Q/P invalidas", criterio de aceptacion de esta fase) y todo observable del
// contrato debe estar en `generated_observables`. Delegado sobre `engine::ffi::forecast_gbm_p`.
ForecastResult forecast_gbm_p(
    const PayoffProgram& program, const GbmPModel& model, std::uint64_t n_paths, std::uint64_t seed
);

// Probabilidad bajo P (Monte Carlo, GBM fisico) de que `event` dispare en la ruta
// (PLAN_PRODUCTS.md §12 Fase 7: "HitProbabilityP"). Misma forma de resultado que
// `hit_probability_gbm` (Q) -- `HitProbabilityResult::measure` distingue cual de las dos se
// obtuvo. Mismo preflight que `forecast_gbm_p`.
HitProbabilityResult hit_probability_gbm(
    const PayoffProgram& program, const GbmPModel& model, const EventId& event, std::uint64_t n_paths,
    std::uint64_t seed
);

// Resultado de distribucion de P&L de una estrategia bajo P (PLAN_PRODUCTS.md §12 Fase 7:
// "distribucion de P&L y expected shortfall de estrategia"). `var`/`es` son PERDIDAS POSITIVAS
// (convencion de riesgo habitual, ver el doc-comment de `engine_core::payoff::PnlDistribution` en
// Rust, que es quien realmente los calcula): `es >= var` siempre, porque el Expected Shortfall
// promedia el propio VaR y todo lo peor que el.
struct PnlDistributionResult {
    double mean = 0.0;
    double std_error = 0.0;
    double var = 0.0;
    double es = 0.0;
    std::uint64_t n_paths = 0;
    ProbabilityMeasure measure = ProbabilityMeasure::PhysicalP;
};

// Distribucion de P&L bajo P (Monte Carlo, GBM fisico) de una estrategia (PLAN_PRODUCTS.md §12
// Fase 7), al nivel de confianza `confidence` (p.ej. `0.95`). Mismo preflight que
// `forecast_gbm_p`; `confidence` fuera de `[0,1)` es un error de evaluacion (lo detecta Rust, ver
// `engine_core::payoff::pnl_distribution_gbm_p`).
PnlDistributionResult pnl_distribution_gbm_p(
    const PayoffProgram& program, const GbmPModel& model, std::uint64_t n_paths, std::uint64_t seed,
    double confidence
);

// --- Fase 11 (items pendientes del cierre documentado en PLAN_PRODUCTS.md §12): cablear
// payoff::api::payoff_sensitivity_gbm_q y payoff::hedge::synthesize_hedge_gbm_q al bridge cxx ---

// Sensibilidad ("Greek") pathwise bajo Q de un `PayoffProgram` respecto de uno de los cuatro
// parametros de `GbmModel` ("spot"/"rate"/"dividend_yield"/"volatility"). Mismo preflight que
// `risk_neutral_price_gbm` (dependencias vs. capacidades del modelo); `greek` fuera de esas
// cuatro cadenas es un error de evaluacion (lo detecta Rust, ver
// `engine_core::payoff::sensitivity::GbmGreek::parse`). Delegado sobre
// `engine::ffi::payoff_sensitivity_gbm_q`, que a su vez decide internamente entre el metodo
// pathwise (Dual) y el fallback bump-and-reval si el contrato contiene `Exercise` -- ver el
// doc-comment de `engine_core::payoff::sensitivity`. `value` puede ser negativo (es una
// derivada, no un precio), a diferencia de `QValuationResult::mean`.
struct SensitivityResult {
    double value = 0.0;
    double std_error = 0.0;
    double ci_low = 0.0;
    double ci_high = 0.0;
    std::uint64_t n_paths = 0;
    ProbabilityMeasure measure = ProbabilityMeasure::RiskNeutralQ;
};

SensitivityResult payoff_sensitivity_gbm(
    const PayoffProgram& program, const GbmModel& model, const std::string& greek, std::uint64_t n_paths,
    std::uint64_t seed
);

// Restricciones opcionales sobre los pesos de `synthesize_hedge_gbm` (PLAN_PRODUCTS.md §12
// Fase 11, items pendientes "liquidez" y "restricciones de tipo LP/QP (posiciones
// minimas/maximas)"). Espejo de `engine_core::payoff::HedgeConstraints` -- ver su doc-comment en
// Rust (`rust/crates/engine-core/src/payoff/hedge.rs`) para el alcance deliberadamente limitado
// (caja por instrumento, no restricciones lineales generales ni enteras; liquidez por escalado
// uniforme, no reoptimizacion).
struct HedgeConstraints {
    // Un `(lower, upper)` por instrumento, mismo orden que `instruments` en `synthesize_hedge_gbm`;
    // vacio = sin restriccion de caja. `+-std::numeric_limits<double>::infinity()` es valido para
    // "sin limite" en un solo lado.
    std::vector<std::pair<double, double>> bounds;
    std::optional<double> max_gross_notional;
};

// Espejo de `engine_core::payoff::HedgeResidualGreeks` (bump-and-reval, numeros aleatorios
// comunes, pesos ya resueltos -- ver su doc-comment en Rust para el porque de no usar el metodo
// pathwise `Dual` aqui).
struct HedgeResidualGreeks {
    double delta = 0.0;
    double rho = 0.0;
    double dividend_yield = 0.0;
    double vega = 0.0;
};

// Espejo de `engine_core::payoff::HedgeResult` (PLAN_PRODUCTS.md §11).
struct HedgeResult {
    std::vector<double> weights;
    // Uno por escenario Monte Carlo compartido: `target_pv[s] + sum_i(weights[i] * design[s][i])`.
    std::vector<double> residuals;
    double residual_mean = 0.0;
    double residual_std = 0.0;
    double residual_max_abs = 0.0;
    std::optional<double> cost;
    std::optional<double> gross_notional;
    std::optional<HedgeResidualGreeks> residual_greeks;
};

// Sintetiza una cobertura bajo GBM/Q (PLAN_PRODUCTS.md §11) para `target` con el universo de
// instrumentos `instruments`. NO es un `IMeasure`: a diferencia de toda otra medida de este
// archivo (un unico `IProduct`), esta operacion necesita un TARGET *y* un universo de N
// instrumentos negociables simultaneamente -- forzarla dentro de `IMeasure::evaluate(model,
// product, market, pricing, execution)` (un unico `product`) exigiria transportar el universo
// completo (specs JSON + precios opcionales) dentro de un `Params` piano, perdiendo la
// tipificacion de `PayoffProgram`/`GbmModel` que ya tiene esta funcion; se expone como funcion
// libre, igual que las demas de `engine/payoff/measures.hpp` antes de que Fase 11 cableara las
// siete medidas de precio/riesgo a `Registry<IMeasure>` (ver el doc-comment de esa seccion en
// `measure.hpp`).
//
// Mismo preflight de observable-generado-por-el-modelo que `risk_neutral_price_gbm`, aplicado al
// target Y a cada instrumento. Delegado sobre `engine::ffi::synthesize_hedge_gbm_q`.
HedgeResult synthesize_hedge_gbm(
    const PayoffProgram& target, const std::vector<const PayoffProgram*>& instruments, const GbmModel& model,
    const std::optional<std::vector<double>>& instrument_prices, double ridge, const HedgeConstraints& constraints,
    bool compute_residual_greeks, std::uint64_t n_paths, std::uint64_t seed
);

} // namespace payoff
} // namespace engine
