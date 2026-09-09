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
