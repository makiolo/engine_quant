#include "engine/bootstrap.hpp"

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
    registries.calibrators.register_type<HullWhite1FCalibrator>("HullWhite1F");
    registries.calibrators.register_type<HullWhite2FCalibrator>("HullWhite2F");
}

} // namespace engine
