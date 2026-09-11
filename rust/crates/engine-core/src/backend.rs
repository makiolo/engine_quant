//! Backend de cómputo (PLAN.md §5.1). En vez de un trait `ComputeBackend` propio, el
//! motor delega enteramente en el sistema de backends de [Burn](https://burn.dev):
//! kernels numéricos (`crate::kernel`, `crate::models`, `crate::products`) se escriben
//! genéricos sobre `burn::tensor::backend::Backend`, y elegir CPU o GPU es literalmente
//! elegir qué alias de tipo de esta página se instancia — ninguna otra parte del motor
//! necesita saberlo (principio de §4: "el backend CPU/GPU debe ser una decisión de
//! configuración, no de diseño del producto/modelo").
//!
//! `f64` como tipo de elemento flotante (en vez del `f32` por defecto de Burn) porque la
//! precisión importa más que el rendimiento en la valoración de derivados: PLAN.md nunca
//! pide entrenar una red neuronal, solo reutiliza el motor tensorial/autodiff de Burn
//! para vectorizar Monte Carlo y diferenciar automáticamente.

/// Backend CPU de referencia (`burn-ndarray`), siempre disponible.
pub type CpuBackend = burn::backend::NdArray<f64>;

/// Backend GPU (`burn-wgpu`, portable: Vulkan/Metal/DX12/WebGPU), tras la feature `gpu`
/// de este crate (no se compila por defecto: `wgpu` tiene un árbol de dependencias y un
/// tiempo de compilación considerables). PLAN.md §6 Fase 5 es cuando este backend pasa a
/// probarse y usarse en serio; aquí ya existe como alias listo para activar.
#[cfg(feature = "gpu")]
pub type GpuBackend = burn::backend::Wgpu<f64>;

/// Envoltorio que añade autodiferenciación en modo reverse (PLAN.md §5.3) a cualquier
/// backend base — sensibilidades vía `crate::sensitivities` se calculan instanciando los
/// modelos/productos con `Autodiff<CpuBackend>` en vez de `CpuBackend` a secas.
pub type Autodiff<B> = burn::backend::Autodiff<B>;

/// Selección de backend en tiempo de ejecución (PLAN.md §7.15: `ExecutionContext` en la capa
/// C++, un parámetro explícito de cada llamada de `crate::api` — ya no un estado global de
/// proceso, ver el historial de §7.12 para el diseño anterior). Los alias de arriba son
/// tipos — la elección real de cuál usar para una llamada concreta es un valor en tiempo de
/// ejecución, porque viene de fuera del proceso Rust (Excel/Python construyen un
/// `ExecutionContext` y lo pasan a `ENGINE.PRICE`): este enum es el puente entre ambos mundos.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ComputeBackend {
    Cpu,
    Gpu,
}

impl ComputeBackend {
    pub fn name(self) -> &'static str {
        match self {
            ComputeBackend::Cpu => "cpu",
            ComputeBackend::Gpu => "gpu",
        }
    }

    /// `true` si este build sabe ejecutar este backend: `Cpu` siempre, `Gpu` solo si este
    /// crate se compiló con la feature `gpu` (PLAN.md §5.1, §7.11) — independientemente de
    /// si la máquina en la que corre tiene o no un adaptador GPU real (eso ya es
    /// responsabilidad de `burn-wgpu` en tiempo de ejecución, ver `examples/gpu_probe.rs`).
    pub fn is_available(self) -> bool {
        match self {
            ComputeBackend::Cpu => true,
            ComputeBackend::Gpu => cfg!(feature = "gpu"),
        }
    }
}

/// Parsea "cpu"/"gpu" (case-insensitive, espacios al borde ignorados) — es la única forma en
/// que `ComputeBackend` entra desde fuera de Rust (un `&str` explícito en `crate::api`, que a
/// su vez viene de `ExecutionContext::backend()` en la capa C++), así que vive junto al enum
/// en vez de en cada frontera.
pub fn parse_backend_name(name: &str) -> Option<ComputeBackend> {
    match name.trim().to_ascii_lowercase().as_str() {
        "cpu" => Some(ComputeBackend::Cpu),
        "gpu" => Some(ComputeBackend::Gpu),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_backend_name_is_case_insensitive() {
        assert_eq!(parse_backend_name("CPU"), Some(ComputeBackend::Cpu));
        assert_eq!(parse_backend_name(" gpu "), Some(ComputeBackend::Gpu));
        assert_eq!(parse_backend_name("tpu"), None);
    }

    #[test]
    fn gpu_is_available_only_with_the_gpu_feature() {
        assert!(ComputeBackend::Cpu.is_available());
        assert_eq!(ComputeBackend::Gpu.is_available(), cfg!(feature = "gpu"));
    }
}
