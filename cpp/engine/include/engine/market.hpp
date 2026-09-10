#pragma once

#include <vector>

namespace engine {

// Snapshot inmutable de una curva de mercado observada -- o fabricada, ver
// synthetic_from_hull_white -- en un instante dado (PLAN.md §7.14): pillars (años desde hoy,
// estrictamente crecientes) + zero_rates (tipos cero de capitalización continua), los mismos
// dos vectores paralelos que ya usa engine::ffi::HullWhiteCalibrationResult por debajo (PLAN.md
// §5.5: "structs planos"). Es solo datos, como Params -- no una jerarquía polimórfica: que el
// mercado sea "falso" o "real" es una cuestión de de dónde salen los números (fabricados aquí
// mismo, leídos de un fichero/feed en el futuro), no de un tipo C++ distinto.
class MarketSnapshot {
public:
    // Lanza std::invalid_argument si pillars/zero_rates no tienen el mismo tamaño, están
    // vacíos, o pillars no es estrictamente creciente (zero_rate()/discount_factor()
    // interpolan linealmente entre pillars consecutivos: sin este invariante no tiene
    // sentido).
    MarketSnapshot(std::vector<double> pillars, std::vector<double> zero_rates);

    const std::vector<double>& pillars() const { return pillars_; }
    const std::vector<double>& zero_rates() const { return zero_rates_; }

    // Tipo cero interpolado linealmente entre los pillars que rodean a t, con extrapolación
    // plana fuera de rango; discount_factor() es exp(-zero_rate(t) * t).
    double zero_rate(double t) const;
    double discount_factor(double t) const;

    // Mercado "falso" (PLAN.md §7.14): fabrica un MarketSnapshot leyendo la propia fórmula
    // cerrada de HullWhite1F (engine::hull_white_zero_coupon_bond) en los pillars dados --
    // útil para probar/demostrar calibración sin depender de datos de mercado reales
    // (round-trip: generar con unos parámetros conocidos, calibrar desde otra estimación
    // inicial, comprobar que se recuperan).
    static MarketSnapshot synthetic_from_hull_white(
        double a, double b, double sigma, double r0, const std::vector<double>& pillars
    );

private:
    std::vector<double> pillars_;
    std::vector<double> zero_rates_;
};

} // namespace engine
