//! Benchmark real CPU vs GPU para PLAN.md §6 Fase 5: ejercitar `GpuBackend` (`burn-wgpu`)
//! sobre el caso base IRS + Hull-White (§5.2) y decidir si conviene activarlo por
//! defecto o dejarlo opcional (feature `gpu`, como hoy).
//!
//! Mide el tiempo de `expected_exposure_profile` (simulación Monte Carlo del tipo corto +
//! revaloración del swap en cada fecha de monitorización, PLAN.md §5.2) sobre
//! `CpuBackend` (`burn-ndarray`) y, cuando se compila con `--features gpu`, también sobre
//! `GpuBackend` (`burn-wgpu`), para una serie de tamaños de `n_paths` — el eje que
//! determina si el paralelismo de GPU compensa su overhead de lanzamiento de kernels.
//!
//! `cargo run -p engine-core --release --features gpu --example gpu_vs_cpu_bench`
//! (sin `--features gpu` solo mide CPU, útil como referencia rápida).

use engine_core::backend::CpuBackend;
use engine_core::exposure::expected_exposure_profile;
use engine_core::models::hull_white::HullWhite1F;
use engine_core::products::irs::IrSwap;
use std::time::{Duration, Instant};

fn scalar<B: burn::tensor::backend::Backend>(
    value: f64,
    device: &burn::tensor::Device<B>,
) -> burn::tensor::Tensor<B, 1> {
    burn::tensor::Tensor::from_data(burn::tensor::TensorData::from([value]), device)
}

/// Construye el caso base (§5.2: IRS 5y anual a la par bajo Hull-White 1F) y mide el
/// tiempo de calcular su perfil de exposición sobre `n_paths` trayectorias.
fn bench_one<B: burn::tensor::backend::Backend<FloatElem = f64>>(
    n_paths: usize,
    device: &burn::tensor::Device<B>,
) -> Duration {
    let a = 0.1;
    let b = 0.03;
    let sigma = 0.01;
    let r0 = 0.02;

    let model: HullWhite1F<B> = HullWhite1F::new(scalar(a, device), scalar(b, device), scalar(sigma, device));
    let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
    let accruals = vec![1.0; 5];
    let fixed_rate = IrSwap::par_rate(scalar(r0, device), 0.0, &payment_times, &accruals, &model);
    let swap: IrSwap<B> = IrSwap {
        notional: scalar(1_000_000.0, device),
        fixed_rate,
        start: 0.0,
        payment_times,
        accruals,
    };
    let monitoring_times = vec![0.0, 1.0, 2.0, 3.0, 4.0];
    let n_steps = 260; // ~1 paso/semana sobre 5 años, mismo orden que la malla que usaba Fase 5

    let start = Instant::now();
    let profile = expected_exposure_profile(&model, &swap, r0, &monitoring_times, n_steps, n_paths, 42, device);
    std::hint::black_box(&profile);
    start.elapsed()
}

fn main() {
    let path_counts = [1_000usize, 10_000, 100_000, 1_000_000];

    println!("{:>10} | {:>14} | {:>14}", "n_paths", "CPU (ndarray)", "GPU (wgpu)");
    println!("{:->10}-+-{:->14}-+-{:->14}", "", "", "");

    for &n_paths in &path_counts {
        let cpu_device = burn::tensor::Device::<CpuBackend>::default();
        // Descarta la primera pasada (calienta el pool de threads/allocator).
        let _ = bench_one::<CpuBackend>(n_paths.min(1_000), &cpu_device);
        let cpu_time = bench_one::<CpuBackend>(n_paths, &cpu_device);

        #[cfg(feature = "gpu")]
        let gpu_time = {
            use engine_core::backend::GpuBackend;
            let gpu_device = burn::tensor::Device::<GpuBackend>::default();
            // La primera llamada paga compilación de shaders/inicialización del adaptador:
            // se descarta para medir solo el coste de cómputo en estado estable.
            let _ = bench_one::<GpuBackend>(n_paths.min(1_000), &gpu_device);
            Some(bench_one::<GpuBackend>(n_paths, &gpu_device))
        };
        #[cfg(not(feature = "gpu"))]
        let gpu_time: Option<Duration> = None;

        let gpu_str = gpu_time
            .map(|d| format!("{:.3?}", d))
            .unwrap_or_else(|| "(sin `gpu`)".to_string());
        println!("{:>10} | {:>14.3?} | {:>14}", n_paths, cpu_time, gpu_str);
    }
}
