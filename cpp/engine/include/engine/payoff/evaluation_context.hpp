#pragma once

#include "engine/payoff/event_state.hpp"
#include "engine/payoff/market_path.hpp"

namespace engine {
namespace payoff {

// Contexto de una evaluación determinista sobre una ruta conocida (PLAN_PRODUCTS.md §5.2).
// `ScenarioEvaluator` no descuenta y no decide Q/P: solo ejecuta el contrato en esta ruta.
struct EvaluationContext {
    const MarketPath& path;
    const FixingStore& historical_fixings;
    RuntimeState& state;
};

} // namespace payoff
} // namespace engine
