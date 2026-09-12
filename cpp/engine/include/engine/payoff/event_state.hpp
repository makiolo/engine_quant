#pragma once

#include <optional>
#include <unordered_map>

#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {

// Estado por ruta de un evento (PLAN_PRODUCTS.md §4.1). Nunca se modifica el AST para
// representar la evolución de una ruta -- este struct vive en `RuntimeState`, mutable y
// efímero, separado del árbol inmutable (§2).
struct EventState {
    bool occurred = false;
    std::optional<TimePoint> first_hit_time;
    std::unordered_map<ObservableId, double> captured_values;
};

// Estado mutable de una única ruta de evaluación (§2, §5.2 `EvaluationContext::state`). Cada
// ruta/hilo posee su propio `RuntimeState`; nunca se comparte entre evaluaciones concurrentes
// del mismo AST inmutable.
class RuntimeState {
public:
    EventState& event_state(const EventId& id) { return states_[id]; }

    const EventState* find_event_state(const EventId& id) const {
        auto it = states_.find(id);
        return it == states_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<EventId, EventState> states_;
};

} // namespace payoff
} // namespace engine
