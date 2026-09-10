//! Sonda mínima para PLAN.md Fase 5 (§6): confirma que `GpuBackend` (`burn-wgpu`) tiene
//! un adaptador GPU real disponible en esta máquina antes de fiarse de cualquier
//! benchmark que lo use. Solo existe tras la feature `gpu` de `engine-core`.
//!
//! `cargo run -p engine-core --features gpu --example gpu_probe`

#[cfg(feature = "gpu")]
fn main() {
    use burn::tensor::{Tensor, TensorData};
    use engine_core::backend::GpuBackend;

    type Device = burn::tensor::Device<GpuBackend>;
    let device = Device::default();
    println!("device seleccionado por burn-wgpu: {device:?}");

    let a: Tensor<GpuBackend, 1> = Tensor::from_data(TensorData::from([1.0f64, 2.0, 3.0]), &device);
    let b = a.clone() + a;
    let result = b.into_data().to_vec::<f64>().unwrap();
    println!("1+1, 2+2, 3+3 en GPU = {result:?}");
    assert_eq!(result, vec![2.0, 4.0, 6.0]);
    println!("OK: GpuBackend ejecuta y devuelve resultados correctos.");
}

#[cfg(not(feature = "gpu"))]
fn main() {
    eprintln!("Compilar con --features gpu para ejecutar esta sonda.");
    std::process::exit(1);
}
