//! Sonda para PLAN.md §7.12: confirma que la selección de backend en tiempo de ejecución de
//! `crate::api` (`set_compute_backend`/`irs_hull_white_exposure_profile`) realmente despacha
//! a `GpuBackend`, no solo que compila — corriendo el mismo caso IRS+Hull-White de §5.2 en
//! `CpuBackend` y en `GpuBackend` vía la API pública (no instanciando los tipos de Burn a
//! mano, a diferencia de `examples/gpu_vs_cpu_bench.rs`) y comparando resultados.
//!
//! `cargo run -p engine-core --features gpu --example backend_dispatch_probe`

#[cfg(feature = "gpu")]
fn main() {
    use engine_core::api::{
        compute_backend_name, irs_hull_white_exposure_profile, is_gpu_backend_available, set_compute_backend,
    };

    assert!(is_gpu_backend_available());
    assert_eq!(compute_backend_name(), "cpu", "cpu debe ser el backend por defecto");

    let call = || {
        irs_hull_white_exposure_profile(
            0.1, 0.03, 0.01, 0.02, 1_000_000.0, 0.0, true, 0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0], vec![1.0; 5],
            &[0.0, 1.0, 2.0], 20_000, 99,
        )
    };

    let cpu_profile = call();
    println!("cpu:  EE={:?}", cpu_profile.ee);

    assert!(set_compute_backend("gpu"));
    assert_eq!(compute_backend_name(), "gpu");
    let gpu_profile = call();
    println!("gpu:  EE={:?}", gpu_profile.ee);

    assert!(set_compute_backend("cpu")); // deja el proceso en el estado por defecto

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
    println!("OK: set_compute_backend(\"gpu\") despacha de verdad a GpuBackend vía crate::api.");
}

#[cfg(not(feature = "gpu"))]
fn main() {
    eprintln!("Compilar con --features gpu para ejecutar esta sonda.");
    std::process::exit(1);
}
