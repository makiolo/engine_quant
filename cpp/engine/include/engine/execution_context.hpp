#pragma once

#include <string>

#include "engine/params.hpp"

namespace engine {

// Cómo ejecutar una llamada a ENGINE.PRICE (PLAN.md §7.15): sustituye por completo el backend
// global de proceso de la Fase 5 (`ENGINE.SET_BACKEND`, PLAN.md §7.12, retirado) -- el
// backend pasa a ser un dato explícito de este contexto, pasado a `ENGINE.PRICE`, en vez de un
// estado mutable que hay que recordar cambiar (y que en Excel exigía recálculo manual tras
// cada cambio). No es polimórfico (una sola forma concreta): `ENGINE.CREATE_EXECUTION` no
// lleva nombre de tipo.
class ExecutionContext {
public:
    // Claves de Params: "backend" (string; "cpu"/"gpu"/"auto", case-insensitive -- "auto" se
    // resuelve aquí mismo a "gpu" si is_gpu_backend_available(), si no a "cpu"; cualquier
    // otro valor lanza std::invalid_argument), "precision" (string; solo "fp64" aceptado hoy
    // -- PLAN.md §5.1 ya documenta f64 como decisión deliberada -- cualquier otro valor lanza
    // con un mensaje claro en vez de ignorarse en silencio).
    explicit ExecutionContext(const Params& params);

    // Siempre "cpu" o "gpu" -- nunca "auto" (ya resuelto en el constructor).
    const std::string& backend() const { return backend_; }
    // Siempre "FP64" hoy.
    const std::string& precision() const { return precision_; }

private:
    std::string backend_;
    std::string precision_;
};

} // namespace engine
