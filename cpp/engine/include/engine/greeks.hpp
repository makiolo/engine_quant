#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/execution_context.hpp"
#include "engine/market.hpp"
#include "engine/measure.hpp"
#include "engine/model.hpp"
#include "engine/params.hpp"
#include "engine/payoff/probability_measure.hpp"
#include "engine/pricing_context.hpp"
#include "engine/product.hpp"

namespace engine {
namespace greeks {

// Catálogo tipado de factores de riesgo (PLAN_GREEKS.md §3.1): parseado desde un string
// namespaced ("model.spot", "curve.parallel", "curve.pillar:3", "credit.hazard_rate",
// "time.theta") antes de evaluar -- una ausencia/typo es error explícito, nunca `0.0` silencioso
// (mismo criterio que `ObservableId` en PLAN_PRODUCTS.md §3.1). Fase 1 solo ejecutaba
// `ModelParameter`; Fase 3 añade `CurveParallel`/`CurvePillar` (ver `compute_greek`), aplicable a
// CUALQUIER producto que descuenta con `MarketSnapshot` (IrSwapProduct y PayoffProduct vía
// `market_snapshot_bridge`), no solo a "DV01". `CreditParameter`/`TimeShift` ya se parsean aquí
// (sintaxis fijada en Fase 0) pero `compute_greek` los rechaza todavía -- llegan en Fases 4-5.
enum class RiskFactorKind { ModelParameter, CurveParallel, CurvePillar, CreditParameter, TimeShift };

struct RiskFactor {
    RiskFactorKind kind = RiskFactorKind::ModelParameter;
    std::string scope;                        // "model" | "curve" | "credit" | "time"
    std::string name;                         // nombre del parámetro; "" si no aplica (parallel/time)
    std::optional<std::size_t> pillar_index;   // solo CurvePillar
};

// Parsea la convención de strings namespaced de PLAN_GREEKS.md §3.1. Lanza std::invalid_argument
// (mensaje que nombra la cadena recibida y las alternativas válidas) si no reconoce el scope o el
// nombre dentro de un scope conocido.
RiskFactor parse_risk_factor(const std::string& text);

// Inversa de parse_risk_factor -- usada en mensajes de error/`GreeksReport::skipped` para que un
// `RiskFactor` sea siempre auditable como el string que lo originó (PLAN_GREEKS.md §13).
std::string to_string(const RiskFactor& risk_factor);

// Orden de la derivada y, opcionalmente, la variable cruzada (PLAN_GREEKS.md §3.2). Fase 1 exige
// order == 1 y cross_factor ausente; compute_greek rechaza cualquier otra combinación de forma
// explícita (Fase 6 la habilita).
struct GreekOrder {
    int order = 1;
    std::optional<RiskFactor> cross_factor;
};

// Método de cálculo (PLAN_GREEKS.md §3.3). Fase 1 solo implementa BumpAndReval; Auto resuelve
// siempre a BumpAndReval porque todavía no existe tabla de capacidades (§5.4, Fase 7). Pedir
// Pathwise/AadReverse explícitamente lanza std::invalid_argument -- nunca degrada en silencio.
enum class GreekMethod { Auto, BumpAndReval, Pathwise, AadReverse };

GreekMethod parse_greek_method(const std::string& text);
std::string to_string(GreekMethod method);

// Petición de una Greek (PLAN_GREEKS.md §3.4): `metric_name` debe ser un nombre ya registrado en
// `Registry<IMeasure>` (p.ej. "PV", "PayoffPriceQ"); `metric_params` son los propios de esa
// medida (vacíos para las tres métricas soportadas en Fase 1: "PV"/"PayoffPriceQ"/
// "PayoffForecastP", ninguna de las cuales toma configuración propia).
struct GreekRequest {
    std::string metric_name;
    Params metric_params;
    RiskFactor risk_factor;
    GreekOrder order;
    GreekMethod method = GreekMethod::Auto;
    std::optional<double> bump_override;
};

// Resultado de una Greek (PLAN_GREEKS.md §3.4). Limitación conocida de Fase 1: `MeasureResult`
// (engine/measure.hpp) no lleva todavía `ProbabilityMeasure` ni `std_error` propios -- `measure`
// se infiere de la convención de sufijo Q/P que ya siguen todas las medidas de payoff
// ("PayoffPriceQ" -> RiskNeutralQ, "PayoffForecastP" -> PhysicalP, cualquier otro nombre ->
// DeterministicScenario) y `std_error` queda siempre ausente hasta que una fase posterior
// propague esa información a través de `MeasureResult`.
//
// Fase 2 (PLAN_GREEKS.md §11): ya no asume que la métrica base sea puramente escalar --
// `MeasureResult` puede llevar `has_scalar`/`scalar` (una probabilidad de hit, la media de una
// distribución de P&L), un perfil temporal `times`/`primary`/`secondary` (un perfil de
// exposición: EE/PFE95 en cada `exposure_time`), o ambos a la vez (`PayoffPnlDistributionP`:
// `scalar` es la media, `primary=[var]`/`secondary=[es]`). `compute_greek` diferencia CADA
// componente presente, punto a punto, con el mismo bump/números aleatorios comunes -- nunca
// obliga al llamante a pedir la Greek de "la media" cuando lo que quiere es la de "el VaR": ambas
// salen del mismo `GreekResult`, `value` (si `has_scalar`) y `primary`/`secondary` (si la métrica
// produce perfil), mismo `times` que la propia métrica base.
struct GreekResult {
    bool has_scalar = false;
    double value = 0.0;
    std::vector<double> times;
    std::vector<double> primary;
    std::vector<double> secondary;
    std::optional<double> std_error;
    GreekOrder order;
    RiskFactor risk_factor;
    GreekMethod method_used = GreekMethod::BumpAndReval;
    payoff::ProbabilityMeasure measure = payoff::ProbabilityMeasure::DeterministicScenario;
    std::optional<double> bump_used;
    std::vector<std::string> warnings;
};

// Motor genérico de bump-and-reval (PLAN_GREEKS.md §4): para `RiskFactorKind::ModelParameter`
// reconstruye `model` con el parámetro desplazado +-h vía `IModel::to_params()`; para
// `CurveParallel`/`CurvePillar` (Fase 3) reconstruye `market` desplazada +-h vía
// `bump_market_parallel`/`bump_market_pillar` (engine/market.hpp) dejando `model` intacto -- en
// ambos casos vuelve a invocar `request.metric_name` (resuelto por `registries.measures`) sobre
// cada copia -- diferencia central (no adelantada), mismo `pricing`/`seed` en ambas evaluaciones
// (números aleatorios comunes, §4.4).
//
// Fase 1: únicamente `request.order.order == 1` sin `cross_factor`,
// `request.risk_factor.kind == RiskFactorKind::ModelParameter`, `request.method` en
// {Auto, BumpAndReval}. Fase 2 levanta la restricción "solo métricas con `has_scalar=true`":
// `up`/`down` deben tener la MISMA forma (mismo `has_scalar`, mismo número de puntos en
// `times`/`primary`/`secondary`) -- una discrepancia de forma entre las dos evaluaciones
// bumpeadas es un error explícito (indicaría que `metric_params` cambia el tamaño del perfil de
// forma no determinista, lo que no debería ocurrir nunca). Fase 3 añade `CurveParallel`/
// `CurvePillar` al conjunto soportado (`CreditParameter`/`TimeShift` siguen rechazados hasta las
// Fases 4-5). Cualquier otra combinación lanza std::invalid_argument con el motivo exacto --
// nunca aproxima en silencio.
//
// Nota de diseño (Fase 3): `Dv01Measure` (measure.hpp) NO se reimplementa sobre esta función --
// su convención numérica es un bump UNIDIRECCIONAL (`bumped - base`, sin dividir por `h`,
// documentado en measure.cpp) mientras que `compute_greek` siempre usa diferencia central
// (`(up-down)/(2h)`, una estimación de derivada); son dos preguntas distintas ("¿cuánto cambia el
// NPV si la curva sube 1pb?" vs "¿cuál es la derivada del NPV respecto de la curva?") que
// coinciden solo aproximadamente. Lo que SÍ comparten, para no duplicar la construcción de la
// curva bumpeada en tres sitios (PLAN_GREEKS.md §4.2), es `bump_market_parallel`/
// `bump_market_pillar` -- ver measure.cpp y payoff/market_snapshot_bridge.cpp.
GreekResult compute_greek(
    const Registries& registries, const GreekRequest& request, const IModel& model,
    const IProduct& product, const MarketSnapshot& market, const PricingContext& pricing,
    const ExecutionContext& execution
);

// Barrido automático de Greeks (PLAN_GREEKS.md §8.5): enumera candidatos de `RiskFactor` sin que
// el llamante los nombre uno a uno. Fase 1: enumera `ModelParameter`, una entrada por cada clave
// `double` de `model.to_params()`. Fase 3 añade `curve.parallel` SIEMPRE como candidato, y
// `curve.pillar:i` por cada pillar de `market.pillars()` solo si `include_curve_buckets` (política
// de enumeración exacta de §8.5, punto 2) -- crédito/tiempo/segundo orden siguen pendientes de las
// Fases 4-6 (`include_second_order` se acepta por compatibilidad con la firma final de §8.5 pero
// todavía no produce candidatos). Un candidato que falle (`compute_greek` lanza) se registra en
// `skipped` con el motivo -- nunca aborta el reporte completo.
struct GreeksReport {
    std::vector<GreekResult> greeks;
    std::vector<std::string> skipped;
};

GreeksReport compute_all_greeks(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution,
    bool include_curve_buckets = false, bool include_second_order = false
);

} // namespace greeks

// Medida genérica "Greek" (PLAN_GREEKS.md §8.1): envoltorio delgado sobre
// `greeks::compute_greek` que aplana su `GreekResult` a `MeasureResult` -- alcanzable desde
// Python/Excel/C ABI sin cambios adicionales porque `price()`/`price_batch`/`price_many`/
// `price_grid` ya resuelven cualquier nombre de `Registry<IMeasure>` (PLAN_PRODUCTS.md §12 Fase
// 8). A diferencia del resto de medidas de measure.hpp, `GreekMeasure::evaluate` necesita
// resolver OTRA medida por nombre (`request.metric_name`) -- por eso guarda una referencia a
// `Registries` capturada en su construcción (ver el registro con `register_factory` en
// bootstrap.cpp en vez de `register_type`, que solo pasa un `Params`).
//
// Convención de prefijo `metric.*` (PLAN_GREEKS.md §8.2, cableada ya en Fase 1, cubierta por
// tests explícitos desde Fase 2): `Params` es un bag plano sin anidamiento, así que la
// configuración propia de la métrica interior ("event" de PayoffHitProbabilityQ/P,
// "exposure_times" de PayoffExposureProfileQ, "confidence" de PayoffPnlDistributionP) se pasa
// como "metric.event"/"metric.exposure_times"/"metric.confidence" -- se reenvía sin el prefijo
// como el `Params` propio de esa medida. Azúcar de nombres sobre el mismo bag plano, no un
// `Params` anidado real. `evaluate` aplana el `GreekResult` (Fase 2: puede llevar `has_scalar`,
// perfil `times`/`primary`/`secondary`, o ambos) a la misma forma de `MeasureResult` sin perder
// ningún componente.
class GreekMeasure : public IMeasure {
public:
    GreekMeasure(const Params& params, const Registries& registries);

    std::string type_name() const override { return "Greek"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    greeks::GreekRequest request_;
    const Registries& registries_;
};

} // namespace engine
