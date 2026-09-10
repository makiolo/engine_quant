#pragma once

#include <string>

#include "engine/market.hpp"
#include "engine/params.hpp"

namespace engine {

// Resultado de una calibración (PLAN.md §7.14): parámetros óptimos + diagnóstico del ajuste.
// optimal_params es un Params (como el que ya consume Registry<IModel>::create) en vez de un
// struct de campos fijos como MeasureResult, porque qué parámetros salen depende de qué
// modelo se calibre -- ExposureProfileMeasure/UnilateralCvaMeasure sí conocen su forma de
// antemano, un calibrador genérico no.
struct CalibrationResult {
    Params optimal_params;
    double rmse = 0.0;
    int iterations = 0;
    bool converged = false;
};

// Interfaz base de todo calibrador registrable (PLAN.md §5.4, mismo patrón que IMeasure):
// toma un mercado observado -- o fabricado, ver MarketSnapshot::synthetic_from_hull_white --
// más una estimación inicial, y devuelve los parámetros que mejor reproducen ese mercado bajo
// el modelo concreto que este calibrador sabe calibrar.
class ICalibrator {
public:
    virtual ~ICalibrator() = default;
    virtual std::string type_name() const = 0;
    virtual CalibrationResult calibrate(const MarketSnapshot& market, const Params& initial_guess) const = 0;
};

// initial_guess: "a", "b" (double, estimación inicial -- se calibran), "sigma", "r0" (double,
// no se calibran, ver engine_core::calibration en el core Rust para el porqué: sigma solo
// entra en el precio del bono cero-cupón como un efecto de segundo orden, mal identificado
// contra únicamente una curva de descuento).
class HullWhite1FCalibrator : public ICalibrator {
public:
    // El constructor con Params existe solo para que Registry<ICalibrator>::register_type sea
    // uniforme con los demás registries (ver bootstrap.cpp); no usa los params.
    explicit HullWhite1FCalibrator(const Params&) {}

    std::string type_name() const override { return "HullWhite1F"; }

    CalibrationResult calibrate(const MarketSnapshot& market, const Params& initial_guess) const override;
};

} // namespace engine
