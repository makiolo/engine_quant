# Fase 6: ejecución CPU y Monte Carlo

La API mantiene Tokio para HTTP y coordinación. El cálculo entra en un pool fijo de workers de
`quant-engine`; `EngineConfig` limita simultáneamente `cpu_units`, `memory_budget_bytes` y
`max_job_memory_bytes`. Cuando la admisión no cabe, el servidor responde `429`/`503` y no intenta
crecer el pool ni materializar más paths.

## Política de threads

En producción se recomienda:

```text
worker_count = cores asignados al proceso
cpu_units = worker_count
nested_parallelism = Disabled
OMP_NUM_THREADS=1
OPENBLAS_NUM_THREADS=1
MKL_NUM_THREADS=1
RAYON_NUM_THREADS=1 (o no inicializar Rayon dentro del kernel)
BURN_NUM_THREADS=1 si el backend lo admite
```

Un provider que declare paralelismo interno debe reservar más `cpu_units` por job y probarlo con
la mezcla real; no se activa globalmente por defecto. No se habilita affinity/NUMA en esta fase.
La configuración de OpenMP/BLAS/Rayon/Burn depende de la imagen y del proveedor enlazado; estas
variables son una barrera de seguridad contra oversubscription, no una garantía portable de que
una librería ignore su propia configuración.

## Reproducibilidad y medición

El RNG particionable versionado es `splitmix64-box-muller-v1` y deriva cada variate de
`(seed, scenario_id, path_id, factor_id, step)`. Por eso cambiar chunks o el orden de workers no
cambia los samples en el mismo build/target. Comparaciones entre backends deben ser estadísticas
(media, varianza, cuantiles e intervalo), no bitwise.

El harness local debe recorrer `worker_count=1,2,4,...,N`, varios tamaños de chunk y una mezcla
de jobs pequeños/grandes, registrando throughput, p50/p95/p99, rechazos, memoria y profundidad de
cola. Bandwidth, LLC misses y NUMA se reportan como `unavailable` si el host no expone contadores;
no se infieren a partir del tiempo. La curva resultante solo describe el host del benchmark y no
es un SLO universal.
