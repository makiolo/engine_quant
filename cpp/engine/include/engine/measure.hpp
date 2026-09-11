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
