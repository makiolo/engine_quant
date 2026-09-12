#pragma once

#include <vector>

#include "engine/payoff/contract.hpp"
#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {
namespace templates {

// Plantillas de barrera sobre `Trigger` (PLAN_PRODUCTS.md §4.2): una barrera no es un nodo
// especial, es un patrón de `Trigger` con predicado/branches concretos. Todas usan
// `Monitoring::Discrete` (continua requiere Brownian bridge, Fase 6) y `Settlement::AtHit`:
// si `on_hit`/`on_miss` ya trae su propio `When`, `Settlement` no tiene efecto (ADR-P0-10); si
// no lo trae (p.ej. un rebate expresado como `Cashflow` desnudo), queda válido de inmediato.
// `latch=true` en las tres: una barrera activada permanece activada (§4.2).

// Up-and-in: `underlying_contract` se activa si `observable` toca o supera `barrier` en algún
// instante de `monitoring_times`; si nunca lo hace, el contrato vale `Zero`.
ContractPtr up_and_in(
    EventId id, ObservableId observable, double barrier, std::vector<TimePoint> monitoring_times,
    ContractPtr underlying_contract
);

// Down-and-out: `underlying_contract` se desactiva (se sustituye por `rebate`) si `observable`
// toca o cae por debajo de `barrier`.
ContractPtr down_and_out(
    EventId id, ObservableId observable, double barrier, std::vector<TimePoint> monitoring_times,
    ContractPtr underlying_contract, ContractPtr rebate
);

// Double knock-out: `underlying_contract` se desactiva (se sustituye por `rebate`) si
// `observable` sale del corredor `[barrier_low, barrier_high]` en cualquier instante de
// `monitoring_times` (predicado `Any(S <= barrier_low, S >= barrier_high)`, §4.2).
ContractPtr double_knock_out(
    EventId id, ObservableId observable, double barrier_low, double barrier_high,
    std::vector<TimePoint> monitoring_times, ContractPtr underlying_contract, ContractPtr rebate
);

// Filtra `full_schedule` a los instantes dentro de `[window_start, window_end]` (ambos
// inclusive, ADR-P0-01), preservando el orden. Para una "window barrier" (§4.2): construir
// `monitoring_times` con esto antes de pasarlo a `up_and_in`/`down_and_out`/`double_knock_out`
// en vez de introducir un nodo/plantilla nuevos.
std::vector<TimePoint> window_schedule(
    const std::vector<TimePoint>& full_schedule, TimePoint window_start, TimePoint window_end
);

} // namespace templates
} // namespace payoff
} // namespace engine
