#include <nanobind/nanobind.h>

#include "engine/engine.hpp"

namespace nb = nanobind;

// Fase 0: cadena de humo del pipeline de build (PLAN.md §7.1).
// El binding 1:1 con la API pública de la capa C++ llega en Fase 3 (PLAN.md §6).
NB_MODULE(engine, m) {
    m.def("ping", &engine::ping);
}
