#include "engine/measure.hpp"

#include <stdexcept>
#include <utility>

#include "engine/engine.hpp"

namespace engine {

namespace {

// Fechas de reseteo del IRS -- start() más todos los payment_times() salvo el último (el
// swap no vuelve a resetear después de su pago final) -- misma definición que
// `IrSwap::is_reset_date` en Rust (`rust/crates/engine-core/src/products/irs.rs`). Sustituye
// el "monitoring_times" que antes llegaba en measure_params (PLAN.md §7.15).
std::vector<double> reset_dates(const IrSwapProduct& product) {
    std::vector<double> dates;
    const std::vector<double>& payment_times = product.payment_times();
    dates.reserve(payment_times.size());
    dates.push_back(product.start());
    for (std::size_t i = 0; i + 1 < payment_times.size(); ++i) {
        dates.push_back(payment_times[i]);
    }
    return dates;
}

} // namespace

MeasureResult ExposureProfileMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext& execution
) const {
    const auto* hw_model = dynamic_cast<const HullWhite1FModel*>(&model);
    if (!hw_model) {
        throw std::invalid_argument(
            "ExposureProfileMeasure: modelo no soportado: " + model.type_name());
    }
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument(
            "ExposureProfileMeasure: producto no soportado: " + product.type_name());
    }

    ExposureProfile profile = irs_hull_white_exposure_profile(
        execution.backend(),
        hw_model->a(), hw_model->b(), hw_model->sigma(), hw_model->r0(),
        irs_product->notional(), irs_product->fixed_rate(), irs_product->use_par_rate(),
        irs_product->start(),
        irs_product->payment_times(), irs_product->accruals(),
        reset_dates(*irs_product),
        pricing.n_steps(), pricing.n_paths(), pricing.seed()
    );

    MeasureResult result;
    result.times = std::move(profile.times);
    result.primary = std::move(profile.ee);
    result.secondary = std::move(profile.pfe_95);
    result.has_scalar = false;
    result.scalar = 0.0;
    return result;
}

MeasureResult UnilateralCvaMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution
) const {
    const auto* hw_model = dynamic_cast<const HullWhite1FModel*>(&model);
    if (!hw_model) {
        throw std::invalid_argument(
            "UnilateralCvaMeasure: modelo no soportado: " + model.type_name());
    }

    // Composición (no duplica la llamada a irs_hull_white_exposure_profile): el tipo de
    // producto se valida dentro de ExposureProfileMeasure::evaluate.
    ExposureProfileMeasure exposure_measure(Params{});
    MeasureResult result = exposure_measure.evaluate(model, product, market, pricing, execution);

    double cva = unilateral_cva_from_exposure(
        execution.backend(),
        hw_model->a(), hw_model->b(), hw_model->sigma(), hw_model->r0(),
        result.times, result.primary,
        market.hazard_rate(), market.recovery_rate()
    );

    result.has_scalar = true;
    result.scalar = cva;
    return result;
}

MeasureResult PresentValueMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext&, const ExecutionContext&
) const {
    const auto* hw_model = dynamic_cast<const HullWhite1FModel*>(&model);
    if (!hw_model) {
        throw std::invalid_argument("PresentValueMeasure: modelo no soportado: " + model.type_name());
    }
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument("PresentValueMeasure: producto no soportado: " + product.type_name());
    }

    double npv = irs_hull_white_npv(
        hw_model->a(), hw_model->b(), hw_model->sigma(), hw_model->r0(),
        irs_product->notional(), irs_product->fixed_rate(), irs_product->use_par_rate(),
        irs_product->start(), irs_product->payment_times(), irs_product->accruals()
    );

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = npv;
    return result;
}

MeasureResult Dv01Measure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext&, const ExecutionContext&
) const {
    const auto* hw_model = dynamic_cast<const HullWhite1FModel*>(&model);
    if (!hw_model) {
        throw std::invalid_argument("Dv01Measure: modelo no soportado: " + model.type_name());
    }
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument("Dv01Measure: producto no soportado: " + product.type_name());
    }

    double delta_r0 = irs_hull_white_npv_delta_r0(
        hw_model->a(), hw_model->b(), hw_model->sigma(), hw_model->r0(),
        irs_product->notional(), irs_product->fixed_rate(), irs_product->use_par_rate(),
        irs_product->start(), irs_product->payment_times(), irs_product->accruals()
    );

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = delta_r0 * 0.0001; // 1 punto básico
    return result;
}

} // namespace engine
