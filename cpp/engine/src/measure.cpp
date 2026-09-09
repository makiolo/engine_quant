#include "engine/measure.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "engine/engine.hpp"

namespace engine {

MeasureResult ExposureProfileMeasure::evaluate(
    const IModel& model, const IProduct& product, const Params& measure_params
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

    const std::vector<double>& monitoring_times = get_vector(measure_params, "monitoring_times");
    auto n_paths = static_cast<std::uint64_t>(get_double(measure_params, "n_paths"));
    auto seed = static_cast<std::uint64_t>(get_double(measure_params, "seed"));

    ExposureProfile profile = irs_hull_white_exposure_profile(
        hw_model->a(), hw_model->b(), hw_model->sigma(), hw_model->r0(),
        irs_product->notional(), irs_product->fixed_rate(), irs_product->use_par_rate(),
        irs_product->start(),
        irs_product->payment_times(), irs_product->accruals(),
        monitoring_times,
        n_paths, seed
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
    const IModel& model, const IProduct& product, const Params& measure_params
) const {
    const auto* hw_model = dynamic_cast<const HullWhite1FModel*>(&model);
    if (!hw_model) {
        throw std::invalid_argument(
            "UnilateralCvaMeasure: modelo no soportado: " + model.type_name());
    }

    // Composición (no duplica la llamada a irs_hull_white_exposure_profile): el tipo de
    // producto se valida dentro de ExposureProfileMeasure::evaluate.
    ExposureProfileMeasure exposure_measure(measure_params);
    MeasureResult result = exposure_measure.evaluate(model, product, measure_params);

    double hazard_rate = get_double(measure_params, "hazard_rate");
    double recovery_rate = get_double(measure_params, "recovery_rate");

    double cva = unilateral_cva_from_exposure(
        hw_model->a(), hw_model->b(), hw_model->sigma(), hw_model->r0(),
        result.times, result.primary,
        hazard_rate, recovery_rate
    );

    result.has_scalar = true;
    result.scalar = cva;
    return result;
}

} // namespace engine
