#pragma once

#include <set>

#include "engine/payoff/ids.hpp"
#include "engine/payoff/probability_measure.hpp"

namespace engine {
namespace payoff {

// Lo que un IModel concreto puede suministrar (PLAN_PRODUCTS.md §6): se compara contra el
// DependencyReport de un contrato ANTES de reservar rutas/paths o lanzar un kernel de
// simulacion, nunca durante la simulacion -- "el compilador debera rechazar, antes de simular,
// un payoff que requiera observables ... que el modelo/algoritmo elegido no pueda suministrar"
// (§0.1). No forma parte de DependencyReport (ver el comentario de esa struct en
// dependency_visitor.hpp): son dos cosas distintas que se comparan entre si, no una sola.
struct ModelCapabilities {
    std::set<ObservableId> generated_observables;
    std::set<ProbabilityMeasure> supported_measures;
    bool supports_joint_paths = false;
    bool supports_continuous_barrier_bridge = false;
    bool supports_early_exercise_regression = false;
};

} // namespace payoff
} // namespace engine
