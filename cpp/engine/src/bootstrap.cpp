#include "engine/bootstrap.hpp"

namespace engine {

void register_builtins(Registries& registries) {
    registries.models.register_type<HullWhite1FModel>("HullWhite1F");
    registries.products.register_type<IrSwapProduct>("IRSwap");
    registries.measures.register_type<ExposureProfileMeasure>("ExposureProfile");
    registries.measures.register_type<UnilateralCvaMeasure>("UnilateralCVA");
    registries.measures.register_type<PresentValueMeasure>("PV");
    registries.measures.register_type<Dv01Measure>("DV01");
    registries.calibrators.register_type<HullWhite1FCalibrator>("HullWhite1F");
}

} // namespace engine
