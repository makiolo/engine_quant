#pragma once

#include <string>
#include <vector>

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

} // namespace engine
