//! Benchmark real CPU vs GPU para el intérprete de payoff bajo Q (PLAN_PRODUCTS.md §12 Fase 11,
//! "vectorización CPU/GPU y benchmark"): hermano de `gpu_vs_cpu_bench.rs` (que mide el caso base
//! IRS + Hull-White, §6 Fase 5), aquí sobre `engine_core::payoff::price_payoff_gbm_q` -- la ruta
//! Monte Carlo GBM que interpreta un `CompiledPayoff` arbitrario (§12 Fase 5/6).
//!
//! Mide el tiempo de `price_payoff_gbm_q("cpu"/"gpu", ...)` (la función pública, no la interna
//! `_on::<B>`) sobre un up-and-in barrier call con una malla de monitorización de 50 puntos --
//! path-dependiente de verdad, a diferencia de una call europea de un único paso: ejercita tanto
//! la simulación GBM vectorizada (`simulate_gbm_columns_at`, la parte que corre en el backend
//! elegido) como la resolución de `Trigger` con corrección de Brownian bridge
//! (`eval::resolve_trigger_states`, PLAN_PRODUCTS.md §4.2). Se usa la función pública en vez de
//! la interna `price_payoff_gbm_q_on::<B>` deliberadamente: esta última es privada al crate
//! (`fn`, no `pub fn`) y un `example` es un binario aparte que solo ve la superficie pública de
//! `engine-core`, igual que `engine-ffi`; además, `compile(spec_json)` recompila el JSON en cada
//! llamada, pero ese coste es constante y despreciable frente a una simulación Monte Carlo de
//! miles/millones de rutas -- medir la función pública mide exactamente lo que un cliente real
//! paga por llamada.
//!
//! `cargo run -p engine-core --release --features gpu --example payoff_gpu_vs_cpu_bench`
//! (sin `--features gpu` solo mide CPU, útil como referencia rápida).

use engine_core::payoff::price_payoff_gbm_q;
use std::time::{Duration, Instant};

/// Mismo contrato que `up_and_in_call_json` en los tests de `payoff::api` (privada a
/// `#[cfg(test)]`, así que se copia/adapta aquí): un up-and-in call (barrera `H`, strike `K`,
/// ambos monitorizados en `monitoring_times`) que paga el intrínseco en `maturity` si el spot
/// toca `H` en algún instante monitorizado, o cero si nunca lo toca.
fn up_and_in_call_json(barrier: f64, strike: f64, maturity: f64, monitoring_times: &[f64]) -> String {
    let times_json = monitoring_times.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(",");
    format!(
        r#"{{
            "schema": "engine.payoff/v1", "id": "UI",
            "contract": {{
                "type": "trigger", "id": "UI",
                "monitoring_times": [{times_json}],
                "condition": {{"type": "greater_equal",
                    "left": {{"type": "current", "observable": "EQ.SPOT.XYZ"}},
                    "right": {{"type": "constant", "value": {barrier}}}}},
                "monitoring": "discrete", "settlement": "at_scheduled_payment", "priority": 0, "latch": true,
                "on_hit": {{"type": "when", "time": {maturity},
                    "child": {{"type": "cashflow", "currency": "USD",
                        "amount": {{"type": "max",
                            "left": {{"type": "sub",
                                "left": {{"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}},
                                "right": {{"type": "constant", "value": {strike}}}}},
                            "right": {{"type": "constant", "value": 0.0}}}}}}}},
                "on_miss": {{"type": "zero"}}
            }}
        }}"#
    )
}

/// Mide el tiempo de precio bajo Q (Monte Carlo) del barrier de arriba sobre `n_paths` rutas.
/// `strike`/`barrier`/`maturity` ya estan horneados en `spec_json` (ver `up_and_in_call_json`);
/// aqui solo hacen falta `s0`/`r`/`q`/`sigma`, los parametros del propio modelo GBM.
fn bench_one(backend: &str, spec_json: &str, n_paths: u64) -> Duration {
    let (s0, r, q, sigma) = (100.0, 0.05, 0.0, 0.2);

    let start = Instant::now();
    let estimate = price_payoff_gbm_q(backend, spec_json, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, 42).unwrap();
    std::hint::black_box(&estimate);
    start.elapsed()
}

fn main() {
    let monitoring_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();
    let spec_json = up_and_in_call_json(120.0, 100.0, 1.0, &monitoring_times);
    let path_counts = [1_000u64, 10_000, 100_000, 1_000_000];

    println!("{:>10} | {:>14} | {:>14}", "n_paths", "CPU (ndarray)", "GPU (wgpu)");
    println!("{:->10}-+-{:->14}-+-{:->14}", "", "", "");

    for &n_paths in &path_counts {
        // Descarta la primera pasada (calienta el pool de threads/allocator).
        let _ = bench_one("cpu", &spec_json, n_paths.min(1_000));
        let cpu_time = bench_one("cpu", &spec_json, n_paths);

        #[cfg(feature = "gpu")]
        let gpu_time = {
            // La primera llamada paga compilación de shaders/inicialización del adaptador: se
            // descarta para medir solo el coste de cómputo en estado estable.
            let _ = bench_one("gpu", &spec_json, n_paths.min(1_000));
            Some(bench_one("gpu", &spec_json, n_paths))
        };
        #[cfg(not(feature = "gpu"))]
        let gpu_time: Option<Duration> = None;

        let gpu_str = gpu_time.map(|d| format!("{:.3?}", d)).unwrap_or_else(|| "(sin `gpu`)".to_string());
        println!("{:>10} | {:>14.3?} | {:>14}", n_paths, cpu_time, gpu_str);
    }
}
