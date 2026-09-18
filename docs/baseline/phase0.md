# Informe Fase 0 — baseline, contratos y presupuestos

**Estado:** artefactos preparados; resultados dependen del perfil ejecutado y de la máquina.

## Alcance

Fase 0 congela la matriz S/M/L, fixtures y oráculos antes de mover ownership. No añade servidor
REST, endpoints estables, sesiones server-side ni job store. El OpenAPI en
[`docs/api/openapi.v1.yaml`](../api/openapi.v1.yaml) es preliminar y lleva `x-contract-status:
preliminary`.

## Artefactos

* Harness reproducible: [`benches/run_baseline.py`](../../benches/run_baseline.py) y el ejemplo
  Rust [`phase0_baseline.rs`](../../rust/crates/engine-core/examples/phase0_baseline.rs).
* Esquema y datasets versionados en [`benches/fixtures/v1`](../../benches/fixtures/v1).
* ADR-001…ADR-016 en [`docs/adr`](../adr).
* OpenAPI preliminar en [`docs/api/openapi.v1.yaml`](../api/openapi.v1.yaml).

## Qué mide el harness

| Benchmark | Estado | Nota |
|---|---|---|
| slice/vector copy | medido | tamaños 0, 8, 1K y 1M; 100M solo `full` |
| bridge/copy | proxy medido | no invoca CXX; gate real pendiente |
| PV escalar vs batch | medido | `engine_core::api::irs_hull_white_npv*` |
| Monte Carlo | medido | `simulate_paths_gbm_q`, semilla 7 |

El harness no finge mediciones de HTTP, scheduler, serialización, context replay, GPU o CXX. Esos
casos están especificados en el plan y requieren su provider/servidor real; el informe debe
incorporar resultados solo cuando se ejecuten en hardware controlado.

## Gates de salida

* Dataset S/M/L versionado y sin materializar el producto cartesiano L: **sí**.
* Métricas repetibles y salida JSON con plataforma/perfil: **sí**, al ejecutar el harness.
* Fixtures de IRS/Hull-White, payoff Q/P, EE/CVA y calibración: **sí**, con oráculos explícitos.
* Presupuesto de memoria por caso: **parcial**. Se fija `market_bytes` y el tamaño de arrays del
  fixture, pero el RSS/VRAM por worker queda pendiente de medir en hardware objetivo.
* Bridge real <1%: **pendiente**; solo existe proxy y no debe usarse como SLO.

## Política de resultados

Los JSON de salida son artefactos de una máquina concreta. Guardar commit, perfil, CPU, RAM,
toolchain y configuración junto al resultado. No comparar wall time entre máquinas ni convertir un
resultado local en presupuesto de producción. Los umbrales de p95/p99, cola, RSS/VRAM,
throughput, tolerancia y rechazo/fallback requieren decisión de negocio/operación antes de Fase 1.

## Verificación de este baseline

En el workspace actual se ejecutó `cargo test -p engine-core --lib`: **239 passed, 0 failed**.
También se ejecutó `python -m pytest benches/test_fixtures.py -q`: **2 passed**, se validó el
OpenAPI con PyYAML y se generó [`baseline-smoke-local.json`](../../benches/results/baseline-smoke-local.json)
con perfil `smoke`. El resultado reporta Windows x86_64, 8 unidades de paralelismo y el commit
capturado por el runner; no incluye RAM ni control de afinidad, por lo que queda fuera de cualquier
SLO de producción.
