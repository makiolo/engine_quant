# Fase 7 — SIMD CPU

El kernel elegido es la reducción de `f64` de `base_values` en el planificador de riesgo. Es el
hot spot numérico contiguo y observable (`base_values.iter().sum()` antes de Fase 7); no se han
vectorizado rutas de scheduler, dominio ni selección de dispositivo.

`quant_engine::simd` expone `CpuVectorCapabilities::detect`, resolución de
`quant_domain::CpuVectorPolicy` y `sum_f64`. `Auto` permanece scalar en producción porque el gate
end-to-end local no fue material ni estable. Una aplicación puede invocar explícitamente la
resolución experimental con umbral después de medir su workload. `Avx2` fuerza esa estrategia cuando está disponible;
`Scalar`, `PortableSimd` y `Avx512` son seguros y caen a la referencia scalar cuando no tienen una
implementación soportada. El kernel AVX2 usa cargas unaligned, por lo que no requiere una
precondición de alineación. Una entrada con NaN o infinito usa scalar para conservar la semántica
de referencia (`+Inf + -Inf` incluido). No se usa FMA: la única diferencia finita admisible es la
re-asociación de la suma, dentro de tolerancia/ULP; `Scalar` permite resultados left-to-right
bit-for-bit.

## Gate local

En la máquina local (`avx2=true, fma=true, avx512f=false`) el harness
`cargo run -p quant-engine --release --example phase7_simd_bench` dio, con 65.536 elementos y 128
repeticiones:

| layout | Scalar | Auto | Avx2 |
| --- | ---: | ---: | ---: |
| aligned | 8.73 ms | 8.73 ms | 4.63 ms |
| offset/unaligned | 8.71 ms | 8.71 ms | 4.50 ms |
| AoS scalar/auto-vectorizer | 5.80 ms | — | — |

El workload end-to-end del planificador de riesgo tardó 1.313 s (Scalar), 1.313 s (Auto scalar)
y 1.397 s (Avx2 forzado) en una ejecución posterior del mismo host. Otra ejecución anterior
observó una ventaja de aproximadamente 3% para Auto/AVX2, pero no fue estable. El portfolio sigue
dominado por las valoraciones Hull–White; por tanto, el gate material end-to-end NO está cumplido.
Auto no habilita SIMD en producción hasta que un benchmark end-to-end estable justifique cambiarlo.

El harness compara layouts aligned/unaligned y mantiene la reducción en SoA (`&[f64]`), que es la
representación usada por riesgo. No se añadió una variante AoS porque el camino productivo no
consume registros AoS: convertirlos añadiría una copia y no sería una comparación del kernel real.
