#include "engine/bootstrap.hpp"

#include <memory>

#include "engine/greeks.hpp"
#include "engine/payoff/payoff_product.hpp"

namespace engine {

void register_builtins(Registries& registries) {
    registries.models.register_type<HullWhite1FModel>("HullWhite1F");
    registries.models.register_type<HullWhite2FModel>("HullWhite2F");
    registries.models.register_type<GbmModel>("GBM");
    registries.models.register_type<GbmPModel>("GBM_P");
    registries.products.register_type<IrSwapProduct>("IRSwap");
    registries.products.register_type<payoff::PayoffProduct>("Payoff");
    registries.measures.register_type<ExposureProfileMeasure>("ExposureProfile");
    registries.measures.register_type<UnilateralCvaMeasure>("UnilateralCVA");
    registries.measures.register_type<PresentValueMeasure>("PV");
    registries.measures.register_type<Dv01Measure>("DV01");
    registries.measures.register_type<PayoffPriceQMeasure>("PayoffPriceQ");
    registries.measures.register_type<PayoffExerciseQMeasure>("PayoffExerciseQ");
    registries.measures.register_type<PayoffHitProbabilityQMeasure>("PayoffHitProbabilityQ");
    registries.measures.register_type<PayoffExposureProfileQMeasure>("PayoffExposureProfileQ");
    registries.measures.register_type<PayoffForecastPMeasure>("PayoffForecastP");
    registries.measures.register_type<PayoffHitProbabilityPMeasure>("PayoffHitProbabilityP");
    registries.measures.register_type<PayoffPnlDistributionPMeasure>("PayoffPnlDistributionP");
    registries.measures.register_type<PayoffSensitivityQMeasure>("PayoffSensitivityQ");
    // "Greek" (PLAN_GREEKS.md §8.1) necesita el propio `Registries` para resolver la métrica
    // interior por nombre (`greeks::compute_greek` llama a `registries.measures.create(...)`) --
    // a diferencia de `register_type<Concrete>`, que solo pasa un `Params` al constructor, esta
    // fábrica captura `registries` (la misma referencia que recibe esta función) para
    // inyectarla en `GreekMeasure`. Sin riesgo de lifetime: `registries` sigue viva mientras
    // exista cualquier medida creada desde ella (misma vida que el resto de `Registry<IMeasure>`).
    registries.measures.register_factory("Greek", [&registries](const Params& params) {
        return std::make_unique<GreekMeasure>(params, registries);
    });
    registries.calibrators.register_type<HullWhite1FCalibrator>("HullWhite1F");
    registries.calibrators.register_type<HullWhite2FCalibrator>("HullWhite2F");
}

} // namespace engine
