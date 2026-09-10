#pragma once

#include <vector>

#include "engine/params.hpp"

namespace engine {

// Snapshot inmutable de una curva de mercado observada -- o fabricada, ver
// synthetic_from_hull_white -- en un instante dado (PLAN.md §7.14/§7.15): pillars (años desde
// hoy, estrictamente crecientes) + zero_rates (tipos cero de capitalización continua), los
// mismos dos vectores paralelos que ya usa engine::ffi::HullWhiteCalibrationResult por debajo
// (PLAN.md §5.5: "structs planos"). Es solo datos, como Params -- no una jerarquía
// polimórfica: que el mercado sea "falso" o "real" es una cuestión de de dónde salen los
// números (fabricados aquí mismo, leídos de un fichero/feed en el futuro), no de un tipo C++
// distinto.
//
// `hazard_rate`/`recovery_rate` (PLAN.md §7.15, añadidos junto con `ENGINE.CALC`): datos de
// crédito observables, opcionales (default 0.0 -- "sin riesgo de default"), que solo consume
// `UnilateralCvaMeasure`. Encajan en `Market` por la misma razón que `pillars`/`zero_rates`:
// son observables desde fuera del `Trade`, no parámetros del modelo ni del producto.
class MarketSnapshot {
public:
    // Lanza std::invalid_argument si pillars/zero_rates no tienen el mismo tamaño, están
    // vacíos, o pillars no es estrictamente creciente (zero_rate()/discount_factor()
    // interpolan linealmente entre pillars consecutivos: sin este invariante no tiene
    // sentido).
    MarketSnapshot(
        std::vector<double> pillars, std::vector<double> zero_rates,
        double hazard_rate = 0.0, double recovery_rate = 0.0
    );

    // Construcción desde un Params (PLAN.md §7.15, `ENGINE.CREATE_MARKET`): mismo patrón que
    // `HullWhite1FModel(const Params&)`/`IrSwapProduct(const Params&)`. Requiere "pillars"/
    // "zero_rates" (vector<double>, mismo largo); "hazard_rate"/"recovery_rate" (double)
    // opcionales, default 0.0 si están ausentes.
    explicit MarketSnapshot(const Params& params);

    const std::vector<double>& pillars() const { return pillars_; }
    const std::vector<double>& zero_rates() const { return zero_rates_; }
    double hazard_rate() const { return hazard_rate_; }
    double recovery_rate() const { return recovery_rate_; }

    // Tipo cero interpolado linealmente entre los pillars que rodean a t, con extrapolación
    // plana fuera de rango; discount_factor() es exp(-zero_rate(t) * t).
    double zero_rate(double t) const;
    double discount_factor(double t) const;

    // Mercado "falso" (PLAN.md §7.14): fabrica un MarketSnapshot leyendo la propia fórmula
    // cerrada de HullWhite1F (engine::hull_white_zero_coupon_bond) en los pillars dados --
    // útil para probar/demostrar calibración sin depender de datos de mercado reales
    // (round-trip: generar con unos parámetros conocidos, calibrar desde otra estimación
    // inicial, comprobar que se recuperan). `hazard_rate`/`recovery_rate` opcionales, default
    // 0.0 (calibración no los usa; solo relevantes si además se quiere ejercitar CVA sobre
    // este mismo mercado fabricado).
    static MarketSnapshot synthetic_from_hull_white(
        double a, double b, double sigma, double r0, const std::vector<double>& pillars,
        double hazard_rate = 0.0, double recovery_rate = 0.0
    );

private:
    std::vector<double> pillars_;
    std::vector<double> zero_rates_;
    double hazard_rate_;
    double recovery_rate_;
};

} // namespace engine
