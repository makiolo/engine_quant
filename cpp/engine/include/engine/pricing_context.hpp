#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/params.hpp"

namespace engine {

// Contexto de valoración de una llamada a ENGINE.PRICE (PLAN.md §7.15): agrupa lo que antes
// vivía disperso en el `measure_params` de cada `ENGINE.EVALUATE` -- `n_paths`/`seed` (una
// simulación Monte Carlo) más `n_steps`, ahora explícito (antes se calculaba internamente
// una malla semanal fija, ver `crate::exposure::expected_exposure_profile`) -- y añade
// `pricing_date`. No es polimórfico (una sola forma concreta, a diferencia de
// IModel/IProduct/IMeasure/ICalibrator): `ENGINE.CREATE_CONTEXT` no lleva nombre de tipo.
//
// **Límite de alcance deliberado**: `pricing_date` se guarda tal cual (el serial numérico
// que ya usa Excel internamente para una fecha, o cualquier convención que decida el
// llamador) sin aritmética de calendario/day-count por detrás -- es metadato, no todavía un
// input que cambie el cálculo. Implementar eso es trabajo futuro (bootstrapping de curvas,
// generación de calendarios de pago, etc. -- ver PLAN.md).
class PricingContext {
public:
    // Lanza std::invalid_argument si "n_paths" o "n_steps" son <= 0 (falla rápido, no en
    // medio de un cálculo). Claves de Params (mismo estilo snake_case que HullWhite1FModel/
    // IrSwapProduct): "pricing_date" (double, default 0.0), "n_paths" (double, requerido, se
    // castea a uint64_t), "n_steps" (double, requerido, se castea a size_t), "seed" (double,
    // requerido, se castea a uint64_t).
    explicit PricingContext(const Params& params);

    double pricing_date() const { return pricing_date_; }
    std::uint64_t n_paths() const { return n_paths_; }
    std::size_t n_steps() const { return n_steps_; }
    std::uint64_t seed() const { return seed_; }

private:
    double pricing_date_;
    std::uint64_t n_paths_;
    std::size_t n_steps_;
    std::uint64_t seed_;
};

} // namespace engine
