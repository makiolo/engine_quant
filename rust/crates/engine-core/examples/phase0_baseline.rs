//! Fase 0: benchmark harness sin Criterion ni dependencias adicionales.
//!
//! Este ejecutable mide operaciones que ya existen en `engine-core` y deja explícito cuándo
//! una medición es solo un proxy (la frontera CXX no se invoca aquí). No convierte estos números
//! en SLOs: el informe debe conservar hardware, perfil y commit junto con el JSON.

use engine_core::api::{irs_hull_white_npv, irs_hull_white_npv_batch, simulate_paths_gbm_q};
use serde_json::{json, Value};
use std::hint::black_box;
use std::time::{Duration, Instant};

fn median_ns(mut samples: Vec<u128>) -> u128 {
    samples.sort_unstable();
    samples[samples.len() / 2]
}

fn timed<F, T>(repetitions: usize, mut f: F) -> u128
where
    F: FnMut() -> T,
{
    let mut samples = Vec::with_capacity(repetitions);
    for _ in 0..repetitions {
        let start = Instant::now();
        black_box(f());
        samples.push(start.elapsed().as_nanos());
    }
    median_ns(samples)
}

fn ms(ns: u128) -> f64 {
    Duration::from_nanos(ns.min(u64::MAX as u128) as u64).as_secs_f64() * 1_000.0
}

fn copy_cases(profile: &str) -> Value {
    let mut lengths = vec![0usize, 8, 1_024, 1_048_576];
    if profile == "full" {
        // 100M doubles is intentionally opt-in: it consumes roughly 1.6 GiB during clone.
        lengths.push(100_000_000);
    }
    let cases: Vec<Value> = lengths
        .into_iter()
        .map(|len| {
            let input = vec![1.0_f64; len];
            let clone_ms = ms(timed(3, || input.clone()));
            let sum_ms = ms(timed(3, || input.iter().copied().sum::<f64>()));
            let preallocated_ms = ms(timed(3, || {
                let mut output = vec![0.0_f64; len];
                output.copy_from_slice(&input);
                output
            }));
            json!({
                "elements": len,
                "bytes": len.saturating_mul(std::mem::size_of::<f64>()),
                "copy_ms": clone_ms,
                "sum_ms": sum_ms,
                "preallocated_output_ms": preallocated_ms
            })
        })
        .collect();
    json!({
        "name": "bridge_copy",
        "status": "measured_proxy",
        "description": "Rust slice/vector copy costs; no CXX call is made",
        "limitation": "Replace with engine-ffi CXX no-op/scalar/vector cases before treating as bridge overhead",
        "cases": cases
    })
}

fn pricing_cases(profile: &str) -> Value {
    let n = if profile == "full" { 1_000 } else { 8 };
    let notionals: Vec<f64> = (0..n).map(|i| 1_000_000.0 + i as f64).collect();
    let fixed_rates: Vec<f64> = (0..n).map(|i| 0.02 + (i % 7) as f64 * 0.0001).collect();
    let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
    let accruals = vec![1.0; 5];
    let scalar_ms = ms(timed(1, || {
        notionals
            .iter()
            .zip(fixed_rates.iter())
            .map(|(&notional, &fixed_rate)| {
                irs_hull_white_npv(
                    0.1,
                    0.03,
                    0.01,
                    0.02,
                    notional,
                    fixed_rate,
                    false,
                    0.0,
                    payment_times.clone(),
                    accruals.clone(),
                )
            })
            .collect::<Vec<_>>()
    }));
    let batch_ms = ms(timed(1, || {
        irs_hull_white_npv_batch(
            0.1,
            0.03,
            0.01,
            0.02,
            notionals.clone(),
            fixed_rates.clone(),
            0.0,
            payment_times.clone(),
            accruals.clone(),
        )
    }));
    json!({
        "name": "pricing_batch",
        "status": "measured",
        "trades": n,
        "scalar_ms": scalar_ms,
        "batch_ms": batch_ms,
        "speedup": scalar_ms / batch_ms.max(f64::EPSILON),
        "operation": "Hull-White 1F IRS PV"
    })
}

fn monte_carlo_case(profile: &str) -> Value {
    let (n_paths, n_steps) = if profile == "full" {
        (10_000_u64, 50_u64)
    } else {
        (1_000, 20)
    };
    let elapsed = timed(1, || {
        simulate_paths_gbm_q("cpu", 100.0, 0.05, 0.01, 0.2, 1.0, n_steps, n_paths, 7)
            .expect("benchmark GBM fixture must be accepted")
    });
    let matrix = simulate_paths_gbm_q("cpu", 100.0, 0.05, 0.01, 0.2, 1.0, n_steps, n_paths, 7)
        .expect("benchmark GBM fixture must be accepted");
    let stride = (n_steps + 1) as usize;
    let terminal_mean = (0..n_paths as usize)
        .map(|path| matrix.paths_flat[path * stride + n_steps as usize])
        .sum::<f64>()
        / n_paths as f64;
    json!({
        "name": "monte_carlo_baseline",
        "status": "measured",
        "backend": "cpu",
        "paths": n_paths,
        "steps": n_steps,
        "elapsed_ms": ms(elapsed),
        "terminal_mean": terminal_mean,
        "seed": 7,
        "operation": "engine_core::api::simulate_paths_gbm_q"
    })
}

fn main() {
    let args = std::env::args().skip(1).collect::<Vec<_>>();
    let profile = args
        .windows(2)
        .find(|pair| pair[0] == "--profile")
        .map(|pair| pair[1].as_str())
        .unwrap_or("smoke");
    assert!(
        matches!(profile, "smoke" | "full"),
        "--profile must be smoke or full"
    );
    let result = json!({
        "schema": "quant.baseline-result/v1",
        "harness": "rust/crates/engine-core/examples/phase0_baseline.rs",
        "profile": profile,
        "status": "measured",
        "platform": {
            "os": std::env::consts::OS,
            "arch": std::env::consts::ARCH,
            "available_parallelism": std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1)
        },
        "benchmarks": [copy_cases(profile), pricing_cases(profile), monte_carlo_case(profile)]
    });
    println!(
        "{}",
        serde_json::to_string_pretty(&result).expect("JSON serialization cannot fail")
    );
}
