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
// anterior de `ENGINE.CREATE_MEASURE`+`ENGINE.EVALUATE` (retirado, ver `engine/calc.hpp`).
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

// NPV determinista del IRS a t=0 bajo Hull-White 1F (PLAN.md §7.15, medida "PV" de
// ENGINE.CALC) -- nueva desde la Fase 8, no existía como medida registrable antes (solo como
// fórmula cerrada interna de IrSwap::npv).
class PresentValueMeasure : public IMeasure {
public:
    explicit PresentValueMeasure(const Params&) {}

    std::string type_name() const override { return "PV"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// Sensibilidad del NPV a un movimiento de 1 punto básico en r0 (PLAN.md §7.15, medida "DV01"
// de ENGINE.CALC): `d(NPV)/d(r0) * 0.0001` vía autodiff (`engine::irs_hull_white_npv_delta_
// r0`). Es una sensibilidad al parámetro del modelo, no una sensibilidad "por bucket" a la
// curva de mercado -- limitación conocida de un modelo de un solo factor.
class Dv01Measure : public IMeasure {
public:
    explicit Dv01Measure(const Params&) {}

    std::string type_name() const override { return "DV01"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;
};

// --- Lote homogéneo (PLAN.md §7.17/§7.19) ---------------------------------------------------
// Equivalentes de lote de los cuatro `compute_*` internos de measure.cpp (mismo despacho por
// `dynamic_cast` a HullWhite1FModel/HullWhite2FModel, "único sitio que conoce ambos modelos a
// la vez"). Declarados aquí (no en el `namespace {}` anónimo de measure.cpp) porque
// `engine::calc_batch` (calc.hpp) necesita llamarlos directamente -- el despacho de lote no
// pasa por `IMeasure`/`Registry<IMeasure>`: con un único producto real (`IrSwapProduct`) no
// hay genericidad real que ganar con un método virtual `evaluate_batch` todavía (mismo
// argumento que ya justificó no generalizar la C ABI de calibración hasta el segundo
// calibrador, PLAN.md §7.18). `irs_products` debe ser un lote ya validado por el llamante:
// mismo calendario (`start`/`payment_times`/`accruals`) y `use_par_rate() == false` en todos
// -- estas cuatro funciones no repiten esa validación.
std::vector<ExposureProfile> compute_exposure_profile_batch(
    const IModel& model, const std::vector<const IrSwapProduct*>& irs_products,
    const PricingContext& pricing, const ExecutionContext& execution
);
std::vector<double> compute_cva_from_exposure_batch(
    const IModel& model, const ExecutionContext& execution,
    const std::vector<ExposureProfile>& profiles,
    double hazard_rate, double recovery_rate
);
std::vector<double> compute_npv_batch(const IModel& model, const std::vector<const IrSwapProduct*>& irs_products);
std::vector<double> compute_npv_delta_r0_batch(const IModel& model, const std::vector<const IrSwapProduct*>& irs_products);

} // namespace engine
