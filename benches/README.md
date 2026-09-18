# Fase 0 — baseline y presupuestos

Este directorio congela los casos de medida antes de cambiar ownership entre Rust y C++. Los
fixtures son inputs y oráculos explícitos; no convierten una tolerancia en un SLO ni corrigen un
bug silenciosamente.

## Ejecutar

Desde la raíz del repositorio:

```text
python benches/run_baseline.py --profile smoke
python benches/run_baseline.py --profile smoke --output benches/results/baseline-local.json
python benches/run_baseline.py --profile full --output benches/results/baseline-full-local.json
```

La vertical REST de Fase 1 tiene dos harnesses complementarios:

```text
python benches/run_rest_overhead.py --iterations 100
python benches/run_rest_overhead.py --url http://127.0.0.1:8080/v1/prices --iterations 100
cargo run -p quant-api --example overhead -- --iterations 100
```

Ambos producen p50/p95/p99 por etapa para concurrencia 1, 8 y `saturation`. El script Python
mide encode/decode y, si se proporciona `--url`, el round-trip HTTP agregado. El ejemplo Rust
mide decode, admisión/cola, compute y encode dentro del proceso. Las etapas no observables se
incluyen en `unavailable_stages`; los resultados son mediciones de la máquina y no fijan SLOs.

El smoke de Fase 9 para comparar cubo materializado frente a reducción acotada es:

```text
python benches/xva_phase9.py --times 2,10 --paths 100,1000 --chunk 32
```

El harness mide bytes estimados de `f64`, reutilización del grafo XVA y scaling de portfolio/path.
El output Arrow permanece marcado `pending`: la API actual devuelve referencias de artefacto JSON y
no se finge una serialización Arrow que aún no existe.

Para medir el pipeline real de Rust (incluyendo shared-vs-separate measures, artefactos y tiempos):

```text
cargo run -p quant-engine --example phase9_xva_bench -- --times 24 --paths 256 --chunk 6
```

Este ejemplo sí invoca `Engine::calculate_xva`; el script Python anterior sigue siendo un proxy
rápido de memoria para explorar escalas sin compilar Rust.

`smoke` mide una matriz acotada y es apropiado para desarrollo. `full` es opt-in: incluye la
medición de copia de 100M `f64` (aproximadamente 1.6 GiB durante un clone) y un Monte Carlo mayor.
El caso L no materializa el producto cartesiano trades × escenarios; se ejecuta por chunks cuando
exista el scheduler que lo soporte.

El resultado `quant.baseline-result/v1` incluye plataforma, perfil y estado `measured`. No se
deben editar a mano los números ni comparar dos máquinas sin conservar el entorno. La medición
`bridge_copy` es deliberadamente `measured_proxy`: actualmente no llama a CXX y solo sirve para
cuantificar el coste de slice/vector en Rust. El benchmark CXX real queda como gate pendiente.

## Matriz y cobertura

* `fixtures/v1/datasets.json`: S/M/L y presupuesto de memoria/shape.
* `fixtures/v1/irs_hull_white.json`: PV IRS/Hull-White y par swap.
* `fixtures/v1/payoff_q_p.json`: oráculos estadísticos Q/P.
* `fixtures/v1/exposure_cva.json`: invariantes EE/PFE/CVA.
* `fixtures/v1/calibration.json`: convergencia de calibración sintética.
* `schema/benchmark_case.schema.json`: contrato de los tamaños de benchmark.

El harness Rust mide PV escalar frente a lote y `simulate_paths_gbm_q` real del core. Las suites
existentes siguen siendo la autoridad para paridad C++/Python/Rust, porque el harness de Fase 0 no
debe duplicar kernels ni declarar paridad sin ejecutar cada provider.

## Umbrales aún no fijados

Los siguientes valores requieren acuerdo de negocio/operación y no se inventan en esta fase:
p95/p99 por operación, espera máxima de cola, RSS/VRAM por worker, throughput de portfolio,
tolerancias por medida/backend, overhead bridge/transporte y política de rechazo/fallback.
