#pragma once

#include <cstdint>
#include <unordered_map>

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
QValuationResult risk_neutral_price_gbm(
    const PayoffProgram& program, const GbmModel& model, std::uint64_t n_paths, std::uint64_t seed
);

} // namespace payoff
} // namespace engine
