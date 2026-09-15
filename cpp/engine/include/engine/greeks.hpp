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
// `ModelParameter`; Fase 3 añadió `CurveParallel`/`CurvePillar` (ver `compute_greek`), aplicable a
// CUALQUIER producto que descuenta con `MarketSnapshot` (IrSwapProduct y PayoffProduct vía
// `market_snapshot_bridge`), no solo a "DV01". Fase 4 añade `CreditParameter`
// (`hazard_rate`/`recovery_rate`), aplicado sobre todo a `UnilateralCVA`/`ExposureProfile`.
// `TimeShift` ya se parsea aquí (sintaxis fijada en Fase 0) pero `compute_greek` lo rechaza
// todavía -- llega en Fase 5.
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

// Orden de la derivada y, opcionalmente, la variable cruzada (PLAN_GREEKS.md §3.2). Fase 1 exigía
// order == 1 y cross_factor ausente; Fase 6 habilita `order == 2` sin `cross_factor` (Gamma/Volga,
// estencil de 3 puntos `(up - 2*base + down)/h²`) y `order == 1` CON `cross_factor` (Vanna/
// cross-gamma, estencil de 4 puntos `(up_up - up_down - down_up + down_down)/(4*h1*h2)`), ambos
// solo para `RiskFactor::kind` en {ModelParameter, CurveParallel, CurvePillar, CreditParameter} --
// `TimeShift` (Theta) queda excluido de orden 2/cruzada (§15: "AAD de segundo orden... fuera de
// alcance", y Theta de segundo orden/cruzada tampoco tiene bump-and-reval implementado en esta
// fase). `order == 2` CON `cross_factor` (tercera derivada) sigue rechazado explícito (§3.2/§15).
struct GreekOrder {
    int order = 1;
    std::optional<RiskFactor> cross_factor;
};

// Método de cálculo (PLAN_GREEKS.md §3.3). Fase 7 puebla la tabla de capacidades (§5.4,
// `pathwise_capabilities()`/`aad_capabilities()` en greeks.cpp): `Auto` usa Pathwise/AadReverse
// cuando (modelo, métrica) está verificado contra bump-and-reval y, para Pathwise, el contrato
// concreto no contiene `ContractOp::Exercise` (§5.1) -- si ninguna especialización aplica, cae a
// BumpAndReval sin que el llamante lo note salvo por `GreekResult::method_used`. Pedir
// Pathwise/AadReverse explícitamente sobre una combinación no verificada (o sobre `order=2`/
// `cross_factor`, que ninguna especialización cubre todavía) lanza std::invalid_argument
// nombrando la razón exacta -- nunca degrada en silencio.
enum class GreekMethod { Auto, BumpAndReval, Pathwise, AadReverse };

GreekMethod parse_greek_method(const std::string& text);
std::string to_string(GreekMethod method);

// Serializacion de GreekResult::measure (PLAN_GREEKS.md §3.4/§9.3, Fase 9): usada por los
// bindings (C ABI/Python) para exponer la ProbabilityMeasure heredada de la metrica base como
// texto, mismo criterio que to_string(RiskFactor)/to_string(GreekMethod) de arriba.
std::string to_string(payoff::ProbabilityMeasure measure);

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
// `CurveParallel`/`CurvePillar`/`CreditParameter` (Fases 3-4) reconstruye `market` desplazada +-h
// vía `bump_market_parallel`/`bump_market_pillar`/`bump_market_credit` (engine/market.hpp)
// dejando `model` intacto; para `TimeShift` (Fase 5) reconstruye `PricingContext` con
// `pricing_date() + dt` dejando `model`/`market` intactos -- en todos los casos vuelve a invocar
// `request.metric_name` (resuelto por `registries.measures`) sobre cada copia. Todo factor SALVO
// `TimeShift` usa diferencia central (no adelantada), mismo `pricing`/`seed` en ambas
// evaluaciones (números aleatorios comunes, §4.4); `TimeShift` usa una diferencia UNIDIRECCIONAL
// `Metric(t+dt) - Metric(t)` (ADR §7.1, ver el cuerpo de la función) porque el tiempo no
// retrocede, y solo está cableado para `metric_name` en {"PV", "PayoffPriceQ"}
// (`metric_supports_time_shift` en greeks.cpp) -- pedirlo sobre otra métrica se rechaza explícito
// en vez de devolver un Theta silenciosamente nulo.
//
// Fase 1: únicamente `request.order.order == 1` sin `cross_factor`,
// `request.risk_factor.kind == RiskFactorKind::ModelParameter`, `request.method` en
// {Auto, BumpAndReval}. Fase 2 levanta la restricción "solo métricas con `has_scalar=true`":
// `up`/`down` deben tener la MISMA forma (mismo `has_scalar`, mismo número de puntos en
// `times`/`primary`/`secondary`) -- una discrepancia de forma entre las dos evaluaciones
// bumpeadas es un error explícito (indicaría que `metric_params` cambia el tamaño del perfil de
// forma no determinista, lo que no debería ocurrir nunca). Fase 3 añadió `CurveParallel`/
// `CurvePillar`; Fase 4 añadió `CreditParameter`; Fase 5 añadió `TimeShift`. Fase 6 añade
// `order == 2` (Gamma/Volga, estencil de 3 puntos que reutiliza `up`/`down` de orden 1 más una
// evaluación extra en el punto base) y `order == 1` con `cross_factor` (Vanna/cross-gamma,
// estencil de 4 puntos, cada uno compuesto encadenando dos bumps vía `bump_state`) para
// `ModelParameter`/`CurveParallel`/`CurvePillar`/`CreditParameter` -- `TimeShift` sigue limitado a
// `order == 1` sin `cross_factor` (ver más arriba). Cualquier otra combinación lanza
// std::invalid_argument con el motivo exacto -- nunca aproxima en silencio.
//
// Nota de diseño (Fase 3): `Dv01Measure` (measure.hpp) NO se reimplementa sobre esta función --
// su convención numérica es un bump UNIDIRECCIONAL (`bumped - base`, sin dividir por `h`,
// documentado en measure.cpp) mientras que `compute_greek` siempre usa diferencia central
// (`(up-down)/(2h)`, una estimación de derivada); son dos preguntas distintas ("¿cuánto cambia el
// NPV si la curva sube 1pb?" vs "¿cuál es la derivada del NPV respecto de la curva?") que
// coinciden solo aproximadamente. Lo que SÍ comparten, para no duplicar la construcción de la
// curva/mercado bumpeado en varios sitios (PLAN_GREEKS.md §4.2), es `bump_market_parallel`/
// `bump_market_pillar`/`bump_market_credit` -- ver measure.cpp y payoff/market_snapshot_bridge.cpp.
GreekResult compute_greek(
    const Registries& registries, const GreekRequest& request, const IModel& model,
    const IProduct& product, const MarketSnapshot& market, const PricingContext& pricing,
    const ExecutionContext& execution
);

// Barrido automático de Greeks (PLAN_GREEKS.md §8.5): enumera candidatos de `RiskFactor` sin que
// el llamante los nombre uno a uno. Fase 1: enumera `ModelParameter`, una entrada por cada clave
// `double` de `model.to_params()`. Fase 3 añadió `curve.parallel` SIEMPRE como candidato, y
// `curve.pillar:i` por cada pillar de `market.pillars()` solo si `include_curve_buckets` (política
// de enumeración exacta de §8.5, punto 2). Fase 4 añadió `credit.hazard_rate`/
// `credit.recovery_rate` SIEMPRE (§8.5 punto 3: "una derivada nula es una respuesta válida,
// distinta de 'no aplica'"). Fase 5 añadió `time.theta` SIEMPRE (§8.5 punto 4): si la métrica
// todavía no honra `PricingContext::pricing_date()` (`metric_supports_time_shift` en greeks.cpp)
// o `dt` cruza un instante que el motor no puede reconstruir sin histórico, `compute_greek` lanza
// y el candidato cae en `skipped` con el motivo -- nunca se omite en silencio ni se computa como
// un Theta cero engañoso. Fase 6 añade, si `include_second_order` (§8.5 punto 5, default `false`),
// la Gamma pura (`order=2`, sin `cross_factor`) de cada parámetro de MODELO (punto 1) -- nunca de
// curva/crédito, y nunca derivadas cruzadas (Vanna/cross-gamma no se enumeran automáticamente,
// solo vía `compute_greek`/`GreekOrder::cross_factor` explícito). Un candidato que falle
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
