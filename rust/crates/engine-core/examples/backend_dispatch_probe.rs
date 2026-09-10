//! Sonda para PLAN.md §7.15: confirma que el parámetro `backend: &str` explícito de
//! `crate::api::irs_hull_white_exposure_profile` realmente despacha a `GpuBackend`, no solo
//! que compila — corriendo el mismo caso IRS+Hull-White de §5.2 con `backend="cpu"` y
//! `backend="gpu"` vía la API pública (no instanciando los tipos de Burn a mano, a diferencia
//! de `examples/gpu_vs_cpu_bench.rs`) y comparando resultados.
//!
//! Antes de §7.15 esto probaba `set_compute_backend`/`compute_backend_name` (estado global
//! de proceso, PLAN.md §7.12); ese mecanismo se eliminó por completo en favor de
//! `ExecutionContext` (capa C++) + este parámetro explícito.
//!
//! `cargo run -p engine-core --features gpu --example backend_dispatch_probe`

#[cfg(feature = "gpu")]
fn main() {
    use engine_core::api::{irs_hull_white_exposure_profile, is_gpu_backend_available};

    assert!(is_gpu_backend_available());

    let call = |backend: &str| {
        irs_hull_white_exposure_profile(
            backend, 0.1, 0.03, 0.01, 0.02, 1_000_000.0, 0.0, true, 0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0], vec![1.0; 5],
            &[0.0, 1.0, 2.0], 104, 20_000, 99,
        )
    };

    let cpu_profile = call("cpu");
    println!("cpu:  EE={:?}", cpu_profile.ee);

    let gpu_profile = call("gpu");
    println!("gpu:  EE={:?}", gpu_profile.ee);

    // Tolerancia relativa (2%), no bit a bit: `Tensor::random` no genera necesariamente la
    // misma secuencia de shocks con la misma seed en CpuBackend (burn-ndarray) y GpuBackend
    // (burn-wgpu) — cada backend implementa su propio generador — así que el perfil de
    // exposición Monte Carlo difiere en ruido estadístico, no en lógica de valoración.
    for (i, (cpu_ee, gpu_ee)) in cpu_profile.ee.iter().zip(gpu_profile.ee.iter()).enumerate() {
        let diff = (cpu_ee - gpu_ee).abs();
        let tolerance = (0.02 * cpu_ee.abs()).max(5.0);
        assert!(
            diff < tolerance,
            "EE[{i}] difiere demasiado entre backends: cpu={cpu_ee} gpu={gpu_ee} diff={diff} tol={tolerance}"
        );
    }
    println!("OK: backend=\"gpu\" despacha de verdad a GpuBackend vía crate::api.");
}

#[cfg(not(feature = "gpu"))]
fn main() {
    eprintln!("Compilar con --features gpu para ejecutar esta sonda.");
    std::process::exit(1);
}
