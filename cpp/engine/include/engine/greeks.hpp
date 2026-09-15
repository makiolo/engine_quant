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
// (mismo criterio que `ObservableId` en PLAN_PRODUCTS.md §3.1). Fase 1 solo EJECUTA
// `ModelParameter` (ver `compute_greek`); los otros cuatro `RiskFactorKind` ya se parsean aquí
// (sintaxis fijada en Fase 0) pero llegan en Fases 3-5.
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
struct GreekResult {
    double value = 0.0;
    std::optional<double> std_error;
    GreekOrder order;
    RiskFactor risk_factor;
    GreekMethod method_used = GreekMethod::BumpAndReval;
    payoff::ProbabilityMeasure measure = payoff::ProbabilityMeasure::DeterministicScenario;
    std::optional<double> bump_used;
    std::vector<std::string> warnings;
};

// Motor genérico de bump-and-reval (PLAN_GREEKS.md §4): reconstruye `model` con el parámetro de
// `request.risk_factor` desplazado +-h vía `IModel::to_params()` y vuelve a invocar
// `request.metric_name` (resuelto por `registries.measures`) sobre cada copia -- diferencia
// central (no adelantada), mismo `pricing`/`seed` en ambas evaluaciones (números aleatorios
// comunes, §4.4).
//
// Fase 1: únicamente `request.order.order == 1` sin `cross_factor`,
// `request.risk_factor.kind == RiskFactorKind::ModelParameter`, `request.method` en
// {Auto, BumpAndReval}, y una medida cuyo `MeasureResult::has_scalar` sea `true`. Cualquier otra
// combinación lanza std::invalid_argument con el motivo exacto -- nunca aproxima en silencio.
GreekResult compute_greek(
    const Registries& registries, const GreekRequest& request, const IModel& model,
    const IProduct& product, const MarketSnapshot& market, const PricingContext& pricing,
    const ExecutionContext& execution
);

// Barrido automático de Greeks (PLAN_GREEKS.md §8.5): enumera candidatos de `RiskFactor` sin que
// el llamante los nombre uno a uno. Fase 1 (versión mínima): solo enumera `ModelParameter`, una
// entrada por cada clave `double` de `model.to_params()` -- curva/crédito/tiempo llegan en Fases
// 3-5 (`include_curve_buckets`/`include_second_order` se aceptan por compatibilidad con la firma
// final de §8.5 pero todavía no producen candidatos adicionales). Un candidato que falle
// (`compute_greek` lanza) se registra en `skipped` con el motivo -- nunca aborta el reporte
// completo.
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
// Convención de prefijo `metric.*` (PLAN_GREEKS.md §8.2): `Params` es un bag plano sin
// anidamiento, así que la configuración propia de la métrica interior (p.ej. "event" de
// PayoffHitProbabilityQ) se pasa como "metric.event" -- se reenvía sin el prefijo como el
// `Params` propio de esa medida. Azúcar de nombres sobre el mismo bag plano, no un `Params`
// anidado real.
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
