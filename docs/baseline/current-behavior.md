# Inventario de defaults y errores observados — Fase 0

Este inventario describe comportamiento existente que debe conservarse o cambiarse de forma
explícita durante la migración. Las referencias son el código actual; no es un contrato REST.

| Área | Comportamiento actual | Evidencia |
|---|---|---|
| backend | `cpu` es el backend efectivo por defecto; un nombre no disponible resuelve defensivamente a CPU en `engine-core` | `rust/crates/engine-core/src/backend.rs`, `api.rs` |
| paths GBM | `n_paths <= 50_000`, `n_steps <= 500`, ambos mayores que cero y maturity finita positiva | `rust/crates/engine-core/src/api.rs` (`SIMULATE_PATHS_MAX_*`) |
| paths GBM | La API devuelve `Result`; shape inválido o límites excedidos son errores antes de simular | `api.rs::check_simulate_paths_limits`, tests de `api` |
| registry C++ | Crear un tipo no registrado lanza `std::out_of_range`; listar devuelve nombres no ordenados | `cpp/engine/include/engine/registry.hpp` |
| ABI C++ | Errores de argumentos se capturan como `engine_abi_last_error`; punteros nulos se rechazan | `cpp/engine/src/abi.cpp`, `cpp/engine/include/engine/abi.h` |
| IRS PV | `irs_hull_white_npv` es determinista CPU; `use_par_rate` calcula el cupón al inicio | `rust/crates/engine-core/src/api.rs` |
| exposure/CVA | EE/PFE/CVA se calculan en API separada; invariantes actuales exigen no negatividad y PFE ≥ EE | `rust/crates/engine-core/src/exposure.rs`, tests |
| RNG | La semilla se pasa explícitamente; tests que comparan salidas de Burn serializan acceso al RNG | `rust/crates/engine-core/src/lib.rs`, `rng_test_lock` |
| Python | El binding actual expone la capa nativa y `quantdesk` añade fachada tipada; los tests de Python son smoke/paridad, no SLO | `clients/python/src`, `clients/python/tests` |

## Decisiones para Fase 1

El adapter REST deberá convertir estos errores a `application/problem+json` con `code` estable sin
exponer mensajes o tipos internos de C++. Cualquier cambio en defaults (backend, límites de paths,
seed o measure Q/P) requiere actualizar la fixture y ejecutar paridad Rust/C++/Python.
