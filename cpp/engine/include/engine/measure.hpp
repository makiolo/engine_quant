#pragma once

#include <string>
#include <vector>

#include "engine/model.hpp"
#include "engine/params.hpp"
#include "engine/product.hpp"

namespace engine {

// Resultado uniforme de cualquier medida (PLAN.md §5.4): times/primary/secondary son el
// perfil temporal (EE/PFE para ExposureProfileMeasure, vacíos para UnilateralCvaMeasure),
// has_scalar/scalar es el agregado escalar (CVA para UnilateralCvaMeasure, sin usar en
// ExposureProfileMeasure).
struct MeasureResult {
    std::vector<double> times;
    std::vector<double> primary;
    std::vector<double> secondary;
    bool has_scalar = false;
    double scalar = 0.0;
};

// Interfaz base de toda medida registrable (PLAN.md §5.4). Limitación conocida del "caso
// base" de Fase 2 (PLAN.md §5.2, solo IRS+Hull-White): evaluate() lanza std::invalid_argument
// si model/product no son del tipo concreto que la medida sabe evaluar (dynamic_cast a
// HullWhite1FModel/IrSwapProduct).
class IMeasure {
public:
    virtual ~IMeasure() = default;
    virtual std::string type_name() const = 0;
    virtual MeasureResult evaluate(const IModel& model, const IProduct& product, const Params& measure_params) const = 0;
};

// measure_params: "monitoring_times" (vector<double>, requerido), "n_paths" (double,
// requerido, se castea a uint64_t), "seed" (double, requerido, se castea a uint64_t).
class ExposureProfileMeasure : public IMeasure {
public:
    // El constructor con Params existe solo para que Registry<IMeasure>::register_type sea
    // uniforme con los demás registries (ver measure.cpp); no usa los params.
    explicit ExposureProfileMeasure(const Params&) {}

    std::string type_name() const override { return "ExposureProfile"; }

    MeasureResult evaluate(const IModel& model, const IProduct& product, const Params& measure_params) const override;
};

// measure_params: igual que ExposureProfileMeasure ("monitoring_times", "n_paths", "seed")
// más "hazard_rate" (double, requerido), "recovery_rate" (double, requerido). Reutiliza
// ExposureProfileMeasure para el perfil EE (composición, no duplica la llamada a
// engine::irs_hull_white_exposure_profile).
class UnilateralCvaMeasure : public IMeasure {
public:
    explicit UnilateralCvaMeasure(const Params&) {}

    std::string type_name() const override { return "UnilateralCVA"; }

    MeasureResult evaluate(const IModel& model, const IProduct& product, const Params& measure_params) const override;
};

} // namespace engine
