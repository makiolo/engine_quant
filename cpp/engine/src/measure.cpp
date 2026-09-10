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

// Interfaz homogénea entre modelos (PLAN.md §7.16): cada medida solo necesita saber, por
// modelo concreto, cómo llamar al core Rust -- el resto de cada Measure::evaluate es idéntico
// sea cual sea el modelo. Estas cuatro funciones son el único sitio de esta capa que conoce la
// existencia de HullWhite1FModel/HullWhite2FModel a la vez; añadir un tercer modelo de tipo
// corto implica una rama más aquí, nada más en el resto del archivo.

ExposureProfile compute_exposure_profile(
    const IModel& model, const IrSwapProduct& irs_product,
    const PricingContext& pricing, const ExecutionContext& execution
) {
    const std::vector<double> monitoring_times = reset_dates(irs_product);
    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return irs_hull_white_exposure_profile(
            execution.backend(),
            hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(),
            irs_product.payment_times(), irs_product.accruals(),
            monitoring_times,
            pricing.n_steps(), pricing.n_paths(), pricing.seed()
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return irs_hull_white_2f_exposure_profile(
            execution.backend(),
            hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(),
            irs_product.payment_times(), irs_product.accruals(),
            monitoring_times,
            pricing.n_steps(), pricing.n_paths(), pricing.seed()
        );
    }
    throw std::invalid_argument("ExposureProfileMeasure: modelo no soportado: " + model.type_name());
}

double compute_cva_from_exposure(
    const IModel& model, const ExecutionContext& execution,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
) {
    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return unilateral_cva_from_exposure(
            execution.backend(),
            hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            times, ee, hazard_rate, recovery_rate
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return unilateral_cva_from_exposure_2f(
            execution.backend(),
            hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            times, ee, hazard_rate, recovery_rate
        );
    }
    throw std::invalid_argument("UnilateralCvaMeasure: modelo no soportado: " + model.type_name());
}

double compute_npv(const IModel& model, const IrSwapProduct& irs_product) {
    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return irs_hull_white_npv(
            hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(), irs_product.payment_times(), irs_product.accruals()
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return irs_hull_white_2f_npv(
            hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(), irs_product.payment_times(), irs_product.accruals()
        );
    }
    throw std::invalid_argument("PresentValueMeasure: modelo no soportado: " + model.type_name());
}

double compute_npv_delta_r0(const IModel& model, const IrSwapProduct& irs_product) {
    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return irs_hull_white_npv_delta_r0(
            hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(), irs_product.payment_times(), irs_product.accruals()
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return irs_hull_white_2f_npv_delta_r0(
            hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(), irs_product.payment_times(), irs_product.accruals()
        );
    }
    throw std::invalid_argument("Dv01Measure: modelo no soportado: " + model.type_name());
}

} // namespace

MeasureResult ExposureProfileMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext& execution
) const {
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument(
            "ExposureProfileMeasure: producto no soportado: " + product.type_name());
    }

    ExposureProfile profile = compute_exposure_profile(model, *irs_product, pricing, execution);

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
    // Composición (no duplica la llamada a compute_exposure_profile): el tipo de producto se
    // valida dentro de ExposureProfileMeasure::evaluate.
    ExposureProfileMeasure exposure_measure(Params{});
    MeasureResult result = exposure_measure.evaluate(model, product, market, pricing, execution);

    double cva = compute_cva_from_exposure(
        model, execution, result.times, result.primary,
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
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument("PresentValueMeasure: producto no soportado: " + product.type_name());
    }

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = compute_npv(model, *irs_product);
    return result;
}

MeasureResult Dv01Measure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext&, const ExecutionContext&
) const {
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument("Dv01Measure: producto no soportado: " + product.type_name());
    }

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = compute_npv_delta_r0(model, *irs_product) * 0.0001; // 1 punto básico
    return result;
}

} // namespace engine
