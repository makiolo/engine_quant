#pragma once

#include <string>
#include <vector>

#include "engine/engine.hpp" // ExposureProfile, usada por compute_exposure_profile_batch
#include "engine/execution_context.hpp"
#include "engine/market.hpp"
#include "engine/model.hpp"
#include "engine/params.hpp"
#include "engine/pricing_context.hpp"
#include "engine/product.hpp"

namespace engine {

// Resultado uniforme de cualquier medida (PLAN.md §5.4): times/primary/secondary son el
// perfil temporal (EE/PFE para ExposureProfileMeasure, vacíos para el resto), has_scalar/
// scalar es el agregado escalar (PV, DV01, CVA).
struct MeasureResult {
    std::vector<double> times;
    std::vector<double> primary;
    std::vector<double> secondary;
    bool has_scalar = false;
    double scalar = 0.0;
};

// Interfaz base de toda medida registrable (PLAN.md §5.4). Desde PLAN.md §7.15, `evaluate`
// recibe `MarketSnapshot`/`PricingContext`/`ExecutionContext` en vez de un `Params` genérico
// de "measure_params" -- estos tres objetos ya tipados sustituyen por completo el flujo
// anterior de `ENGINE.CREATE_MEASURE`+`ENGINE.EVALUATE` (retirado, ver `engine/price.hpp`).
// Limitación conocida del "caso base" de Fase 2 (PLAN.md §5.2, solo IRS+Hull-White):
// evaluate() lanza std::invalid_argument si model/product no son del tipo concreto que la
// medida sabe evaluar (dynamic_cast a HullWhite1FModel/IrSwapProduct).
class IMeasure {
public:
    virtual ~IMeasure() = default;
    virtual std::string type_name() const = 0;
    virtual MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const = 0;
};

// Perfil de exposición esperada (EE) y potencial futura al 95% (PFE95) -- las fechas de
// monitorización ya no son un parámetro externo (antes "monitoring_times" en measure_params):
// se auto-derivan de las propias fechas de reseteo del `IrSwapProduct` (`start()` + todos los
// `payment_times()` salvo el último -- misma definición que `IrSwap::is_reset_date` en Rust,
// PLAN.md §7.15).
class ExposureProfileMeasure : public IMeasure {
public:
    // El constructor con Params existe solo para que Registry<IMeasure>::register_type sea
    // uniforme con los demás registries (ver measure.cpp); no usa los params.
    explicit ExposureProfileMeasure(const Params&) {}

    std::string type_name() const override { return "ExposureProfile"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// CVA unilateral (hazard rate/recovery rate planos, leídos de `market` -- PLAN.md §7.15,
// antes "hazard_rate"/"recovery_rate" en measure_params). Reutiliza ExposureProfileMeasure
// para el perfil EE (composición, no duplica la llamada a
// engine::irs_hull_white_exposure_profile).
class UnilateralCvaMeasure : public IMeasure {
public:
    explicit UnilateralCvaMeasure(const Params&) {}

    std::string type_name() const override { return "UnilateralCVA"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// NPV determinista del IRS a t=0, replicado en bonos cero-cupón sobre la curva de descuento
// OBSERVADA (`MarketSnapshot::discount_factor`, PLAN_REAPI.md §6 Fase 4) -- ya NO usa el
// modelo (`model` es un parámetro sin nombre en `evaluate()`, PLAN.md §7.15 fija la firma con
// los cinco argumentos aunque no todos se usen): la PV de un swap vainilla es, en efecto, solo
// función de la curva de mercado, no del tipo corto simulado. Antes (hasta PLAN.md §7.15)
// llamaba a `irs_hull_white_npv` (Rust, dependiente de HullWhite1F/2F) -- esa ruta sigue
// existiendo en Rust para las rutas Monte Carlo (`ExposureProfileMeasure`/
// `UnilateralCvaMeasure`, que revaloran en fechas FUTURAS donde no hay curva observable), pero
// PV ya no la llama. Swaps "a la par" (`use_par_rate() == true`) calculan su tipo fijo a
// mercado con esta misma curva -- `PV ≈ 0` se conserva algebraicamente sea cual sea la curva.
class PresentValueMeasure : public IMeasure {
public:
    explicit PresentValueMeasure(const Params&) {}

    std::string type_name() const override { return "PV"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// NPV determinista del IRS bajo el MODELO Hull-White (PLAN_GREEKS.md §11 Fase 7), a diferencia
// de `PresentValueMeasure` ("PV"), que desde PLAN_REAPI.md §6 Fase 4 descuenta por la curva
// OBSERVADA de `MarketSnapshot` y ya NO depende del modelo en absoluto -- por eso `model.r0`/
// `model.a`/`model.b`/`model.sigma` son sistematicamente CERO via bump-and-reval sobre "PV"
// (ver `GreeksFase1Test.ComputeAllGreeksOnIrsPvIsZeroForHullWhiteAndCreditButNonZeroForCurveParallel`
// en test_greeks.cpp), un resultado correcto pero que deja sin metrica registrada alguna que SI
// dependa del modelo para adjuntarle una Greek. `HullWhiteModelNpvMeasure` llena ese hueco:
// `evaluate()` ignora `market` (sin nombre, mismo patron que `PresentValueMeasure` ignora
// `model`) y reprecia el swap directamente con los parametros del modelo (`a`/`b`/`sigma`/`r0`,
// mas `eta`/`rho` si es HullWhite2F) vía `engine::irs_hull_white_npv`/`irs_hull_white_2f_npv` --
// la misma funcion determinista que ya usan `irs_hull_white_npv_delta_r0`/
// `irs_hull_white_npv_all_greeks` (Rust, AAD reverse-mode) para construir el grafo diferenciable
// que la tabla de capacidades de `engine::greeks` cablea como `method=aad` (greeks.cpp). Solo
// `IrSwapProduct` + `HullWhite1FModel`/`HullWhite2FModel` -- cualquier otra combinacion lanza
// `std::invalid_argument`, mismo criterio que el resto de medidas de este archivo.
class HullWhiteModelNpvMeasure : public IMeasure {
public:
    explicit HullWhiteModelNpvMeasure(const Params&) {}

    std::string type_name() const override { return "HullWhiteModelNpv"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// Sensibilidad del NPV (misma réplica que PresentValueMeasure, PLAN_REAPI.md §6 Fase 4) a un
// bump PARALELO de `bump` en todos los `zero_rates` de la curva -- bump-and-reval, no AAD: se
// calcula el tipo fijo efectivo UNA vez bajo la curva base (el contrato del swap no cambia al
// mover el mercado) y se repriva con `MarketSnapshot` base y bumpeada. Reemplaza la
// sensibilidad anterior (`d(NPV)/d(r0)` vía autodiff en Rust, PLAN.md §7.15): DV01 dejó de ser
// una sensibilidad al parámetro del modelo -- ya no depende del modelo en absoluto (`model` sin
// usar en `evaluate()`, misma razón que PresentValueMeasure). `bump` es un `Params` opcional
// (PLAN_REAPI.md §6 Fase 3, propuesta 3 -- primera medida con configuración real: `Registry<
// IMeasure>::create("DV01", {{"bump", 0.0002}})` da una sensibilidad distinta de la de
// `create("DV01")`), leído en el constructor porque `evaluate()` no recibe un `Params` propio.
//
// `bucketed` (PLAN_REAPI.md §6 Fase 5, construida sobre la Fase 4): en vez de un único bump
// PARALELO de toda la curva, bumpea cada `zero_rates[i]` INDIVIDUALMENTE (uno a la vez, mismo
// `bump`) y devuelve un delta por pillar (`times`=`market.pillars()`, `primary`=deltas,
// `has_scalar=false`) -- misma forma de `MeasureResult` que ya usa `ExposureProfileMeasure`,
// sin inventar un tipo de resultado nuevo. El DV01 "parcial" (escalar, `bucketed=false`) es la
// suma de estos deltas -- ver el test de consistencia en `test_registry.cpp`.
class Dv01Measure : public IMeasure {
public:
    explicit Dv01Measure(const Params& params) :
        bump_(get_double(params, "bump", 0.0001)), bucketed_(get_bool(params, "bucketed", false)) {}

    std::string type_name() const override { return "DV01"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    double bump_;
    bool bucketed_;
};

// --- Monte Carlo de PayoffProduct bajo Q/P, cableado a Registry<IMeasure>/Engine.price ------
// (PLAN_PRODUCTS.md §12: las funciones libres de `engine/payoff/measures.hpp`
// -- `risk_neutral_price_gbm`/`exercise_price_gbm`/`hit_probability_gbm`/
// `payoff_exposure_profile_gbm`/`forecast_gbm_p`/`pnl_distribution_gbm_p`/`payoff_sensitivity_gbm`
// -- ya existían y ya cruzaban a Rust vía el bridge cxx, pero solo eran alcanzables llamándolas
// directamente desde C++ (tests); estas ocho medidas son el único paso que faltaba para que `Engine.price(...)`
// las resuelva por nombre igual que "PV"/"DV01"/"ExposureProfile" -- y, por construcción de
// `price()`/`price_batch`/`price_many`/`price_grid`/Python/Excel (que resuelven cualquier
// nombre presente en `Registry<IMeasure>` sin lista cerrada aparte, salvo el `price_batch` de
// IRS), quedan automáticamente alcanzables desde ahí también sin tocar esas capas.
//
// `n_paths`/`seed` salen de `PricingContext` (mismo origen que las medidas Monte Carlo de IRS,
// `ExposureProfileMeasure`/`UnilateralCvaMeasure` arriba) -- nunca de un `Params` propio de la
// medida, por consistencia. `ExecutionContext::backend()` NO se usa todavía: el bridge cxx de
// payoff (`engine-ffi`) siempre ejecuta en CPU pase lo que pase (ver el doc-comment de
// `payoff::risk_neutral_price_gbm` y la nota de Fase 11 en PLAN_PRODUCTS.md) -- limitación ya
// documentada, no un descarte silencioso introducido aquí.
//
// Nombres NUEVOS y explícitos (en vez de ramas adicionales dentro de "PV"/"ExposureProfile"):
// un Monte Carlo GBM bajo Q/P es una medida distinta de la réplica determinista de curva que ya
// hacen "PV"/"DV01" para `PayoffProduct` (`market_snapshot_bridge.hpp`) -- mezclarlas bajo el
// mismo nombre, despachando por tipo de modelo, sería más difícil de razonar y de descubrir
// (`Engine.list_measures()`) que dos nombres separados.

// Precio bajo Q (Monte Carlo, GBM) de un `PayoffProduct` (PLAN_PRODUCTS.md §12 Fase 5).
// `product` debe ser `payoff::PayoffProduct`, `model` debe ser `GbmModel` -- cualquier otra
// combinación lanza `std::invalid_argument` (mismo criterio que el resto de medidas de este
// archivo). El preflight de dependencias-vs-capacidades vive en `risk_neutral_price_gbm`.
class PayoffPriceQMeasure : public IMeasure {
public:
    explicit PayoffPriceQMeasure(const Params&) {}

    std::string type_name() const override { return "PayoffPriceQ"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// Precio bajo Q de un `PayoffProduct` con un derecho de `Exercise` (PLAN_PRODUCTS.md §10, Fase
// 9), más el diagnóstico de la política de Longstaff-Schwartz exportable ("explain muestra la
// política", criterio de aceptación de esa fase): `times`/`primary`/`secondary` llevan, por
// fecha de decisión (orden ascendente), la propia fecha, la fracción de rutas que ejercitó
// (`primary`) y el número de rutas in-the-money en esa fecha (`secondary`, casteado a
// `double`) -- mismo hueco de forma que `ExposureProfileMeasure` (perfil temporal +
// `has_scalar`/`scalar` para el precio agregado).
class PayoffExerciseQMeasure : public IMeasure {
public:
    explicit PayoffExerciseQMeasure(const Params&) {}

    std::string type_name() const override { return "PayoffExerciseQ"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// Probabilidad bajo Q de que un `Trigger` del contrato dispare (PLAN_PRODUCTS.md §12 Fase 6).
// `event` (obligatorio, `Params{{"event", std::string(...)}}`) identifica el `EventId` del
// `Trigger` a consultar -- mismo estilo que `Dv01Measure::bump_` (configuración leída en el
// constructor porque `evaluate()` no recibe un `Params` propio).
class PayoffHitProbabilityQMeasure : public IMeasure {
public:
    explicit PayoffHitProbabilityQMeasure(const Params& params) : event_(get_string(params, "event")) {}

    std::string type_name() const override { return "PayoffHitProbabilityQ"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    std::string event_;
};

// Perfil de exposición pathwise bajo Q de un `PayoffProduct` (PLAN_PRODUCTS.md §12 Fase 6).
// `exposure_times` (obligatorio, `Params{{"exposure_times", std::vector<double>{...}}}`) --
// mismo `times`/`primary`(EE)/`secondary`(PFE95) que `ExposureProfileMeasure`, reutilizando
// `engine::ExposureProfile` sin inventar una forma de resultado nueva (§5.5).
class PayoffExposureProfileQMeasure : public IMeasure {
public:
    explicit PayoffExposureProfileQMeasure(const Params& params) : exposure_times_(get_vector(params, "exposure_times")) {}

    std::string type_name() const override { return "PayoffExposureProfileQ"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    std::vector<double> exposure_times_;
};

// CVA unilateral (PLAN_IMPROVE_NOTEBOOK.md Fase 5) de un `PayoffProduct` bajo `GbmModel`:
// reutiliza `PayoffExposureProfileQMeasure` internamente para el perfil EE (misma composición
// que `UnilateralCvaMeasure` con `ExposureProfileMeasure` arriba, mismo parámetro obligatorio
// `exposure_times`, `Params{{"exposure_times", std::vector<double>{...}}}`) y aplica la MISMA
// fórmula de integración de supervivencia hazard-rate-constante/recovery-rate-constante --
// `(1-R) * Σ EE_i · ΔPD_i · DF_i` -- que ya implementa `UnilateralCvaMeasure` para IRS.
//
// Deliberadamente NO llama a la función libre `compute_cva_from_exposure` de arriba: esa
// función descuenta con la dinámica ANALÍTICA propia de HullWhite1F/2F
// (`model.zero_coupon_bond(a,b,sigma,r0,t)`, vía el bridge Rust `unilateral_cva_from_exposure`)
// -- no existe (ni tiene sentido pedir) un equivalente de esa dinámica para `GbmModel`. En su
// lugar el descuento sale de `MarketSnapshot::discount_factor` (la curva OBSERVADA) -- mismo
// criterio que `PresentValueMeasure`/`Dv01Measure` ya usan para cualquier `PayoffProduct`
// (PLAN_REAPI.md §6 Fase 4): un `PayoffProduct` nunca descuenta con la dinámica propia de un
// modelo, GBM incluido, solo con la curva de mercado. La agregación en sí (`compute_cva_from_
// exposure_market`, measure.cpp) SÍ es una segunda implementación de la misma fórmula
// matemática que `unilateral_cva_with_discount` (Rust) -- deliberada y documentada, no un
// descuido: no hay Rust bridge genérico (independiente de HullWhite) que exponer aquí sin
// ampliar el alcance de esta fase a `rust/crates/engine-core` (fuera de los archivos tocados
// por PLAN_IMPROVE_NOTEBOOK.md Fase 5).
//
// `hazard_rate`/`recovery_rate` se leen de `market` (`market.hazard_rate()`/
// `market.recovery_rate()`), NO de un `Params` propio de esta medida -- mismo origen que ya usa
// `UnilateralCvaMeasure` (PLAN.md §7.15: son datos de crédito observables desde fuera del
// contrato, no parámetros de la medida ni del producto). Quien quiera explorar una curva de
// hazard rate (como hace hoy `06_exposure_cva_portfolio.ipynb` con un grid en Python) construye
// un `MarketSnapshot` distinto por punto del grid, igual que ya hace para el IRS con
// `UnilateralCVA`.
class PayoffUnilateralCvaQMeasure : public IMeasure {
public:
    explicit PayoffUnilateralCvaQMeasure(const Params& params) : exposure_times_(get_vector(params, "exposure_times")) {}

    std::string type_name() const override { return "PayoffUnilateralCvaQ"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    std::vector<double> exposure_times_;
};

// "Forecast" bajo P de un `PayoffProduct` (PLAN_PRODUCTS.md §12 Fase 7): NUNCA descontado --
// `model` debe ser `GbmPModel` (un `GbmModel` de Fase 5 se rechaza, "el motor rechaza
// combinaciones Q/P inválidas", ver el preflight de `forecast_gbm_p`).
class PayoffForecastPMeasure : public IMeasure {
public:
    explicit PayoffForecastPMeasure(const Params&) {}

    std::string type_name() const override { return "PayoffForecastP"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// Probabilidad bajo P de que un `Trigger` dispare (PLAN_PRODUCTS.md §12 Fase 7). Mismo `event`
// obligatorio que `PayoffHitProbabilityQMeasure`; `model` debe ser `GbmPModel`.
class PayoffHitProbabilityPMeasure : public IMeasure {
public:
    explicit PayoffHitProbabilityPMeasure(const Params& params) : event_(get_string(params, "event")) {}

    std::string type_name() const override { return "PayoffHitProbabilityP"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    std::string event_;
};

// Distribución de P&L de una estrategia bajo P (PLAN_PRODUCTS.md §12 Fase 7): `scalar`/`mean`
// es la media, `primary=[var]`/`secondary=[es]` (un único elemento cada uno, `es >= var`
// siempre -- ver el doc-comment de `PnlDistributionResult`). `confidence` opcional (default
// `0.95`, mismo estilo que `Dv01Measure::bump_`).
class PayoffPnlDistributionPMeasure : public IMeasure {
public:
    explicit PayoffPnlDistributionPMeasure(const Params& params) : confidence_(get_double(params, "confidence", 0.95)) {}

    std::string type_name() const override { return "PayoffPnlDistributionP"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    double confidence_;
};

// Sensibilidad ("Greek") pathwise bajo Q de un `PayoffProduct` respecto de uno de los cuatro
// parámetros de `GbmModel` (PLAN_PRODUCTS.md §12 Fase 11, item pendiente "cablear
// payoff::api::payoff_sensitivity_gbm_q ... al bridge cxx" -- ver el doc-comment de esa sección
// en `engine/payoff/measures.hpp`). `greek` (obligatorio, `Params{{"greek",
// std::string("spot"|"rate"|"dividend_yield"|"volatility")}}`) -- mismo estilo que `event` en
// `PayoffHitProbabilityQMeasure`. `scalar` es la derivada (puede ser negativa), no un precio.
class PayoffSensitivityQMeasure : public IMeasure {
public:
    explicit PayoffSensitivityQMeasure(const Params& params) : greek_(get_string(params, "greek")) {}

    std::string type_name() const override { return "PayoffSensitivityQ"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;

private:
    std::string greek_;
};

// --- Lote homogéneo (PLAN.md §7.17/§7.19) ---------------------------------------------------
// Equivalentes de lote de los `compute_*` internos de measure.cpp. Declarados aquí (no en el
// `namespace {}` anónimo de measure.cpp) porque `engine::price_batch` (price.hpp) necesita
// llamarlos directamente -- el despacho de lote no pasa por `IMeasure`/`Registry<IMeasure>`:
// con un único producto real (`IrSwapProduct`) no hay genericidad real que ganar con un método
// virtual `evaluate_batch` todavía (mismo argumento que ya justificó no generalizar la C ABI
// de calibración hasta el segundo calibrador, PLAN.md §7.18). `irs_products` debe ser un lote
// ya validado por el llamante: mismo calendario (`start`/`payment_times`/`accruals`) y
// `use_par_rate() == false` en todos -- estas funciones no repiten esa validación.
//
// `compute_exposure_profile_batch`/`compute_cva_from_exposure_batch` siguen despachando por
// `dynamic_cast` a HullWhite1FModel/HullWhite2FModel ("único sitio que conoce ambos modelos a
// la vez") porque siguen dependiendo del modelo (Monte Carlo). `compute_npv_batch`/
// `compute_dv01_batch` ya NO (PLAN_REAPI.md §6 Fase 4, ver PresentValueMeasure/Dv01Measure).
std::vector<ExposureProfile> compute_exposure_profile_batch(
    const IModel& model, const std::vector<const IrSwapProduct*>& irs_products,
    const PricingContext& pricing, const ExecutionContext& execution
);
std::vector<double> compute_cva_from_exposure_batch(
    const IModel& model, const ExecutionContext& execution,
    const std::vector<ExposureProfile>& profiles,
    double hazard_rate, double recovery_rate
);
// PLAN_REAPI.md §6 Fase 4: a diferencia de las tres de arriba, estas dos YA NO dependen del
// modelo -- PV/DV01 de un swap vainilla son función únicamente de la curva de descuento
// observada (`MarketSnapshot`), no del tipo corto simulado. `irs_products` sigue siendo un
// lote ya validado por `require_homogeneous_irs_batch` (`use_par_rate() == false` en todos,
// así que a diferencia de `price()` estas dos no necesitan calcular un par rate).
std::vector<double> compute_npv_batch(const MarketSnapshot& market, const std::vector<const IrSwapProduct*>& irs_products);

// DV01 por lote (PLAN_REAPI.md §6 Fase 4): ya no es `d(NPV)/d(r0)` (el modelo ni interviene) --
// es bump-and-reval de la curva de descuento: bump paralelo de todos los `zero_rates` en
// `bump`, reprecio con la `MarketSnapshot` bumpeada, diferencia contra el precio base.
std::vector<double> compute_dv01_batch(
    const MarketSnapshot& market, const std::vector<const IrSwapProduct*>& irs_products, double bump
);

// Bucketed DV01 por lote (PLAN_REAPI.md §6 Fase 5): un vector de deltas (uno por pillar de
// `market`) por trade, mismo bump-and-reval que `compute_dv01_batch` pero pillar a pillar en
// vez de un bump paralelo -- equivalente de lote de `Dv01Measure::evaluate` con
// `bucketed=true`.
std::vector<std::vector<double>> compute_dv01_bucketed_batch(
    const MarketSnapshot& market, const std::vector<const IrSwapProduct*>& irs_products, double bump
);

} // namespace engine
