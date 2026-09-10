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

/// Selección de backend en tiempo de ejecución (PLAN.md §7.12: "los clientes deben poder
/// elegir CPU o GPU"). Los alias de arriba son tipos — la elección real de cuál usar para
/// una llamada concreta de `crate::api` es un valor en tiempo de ejecución, porque viene de
/// fuera del proceso Rust (una UDF de Excel, un `with` de Python): este enum más el estado
/// global de abajo son el puente entre ambos mundos.
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

    fn from_code(code: u8) -> Self {
        match code {
            1 => ComputeBackend::Gpu,
            _ => ComputeBackend::Cpu,
        }
    }

    fn code(self) -> u8 {
        match self {
            ComputeBackend::Cpu => 0,
            ComputeBackend::Gpu => 1,
        }
    }
}

/// Parsea "cpu"/"gpu" (case-insensitive, espacios al borde ignorados) — es la única forma en
/// que `ComputeBackend` entra desde fuera de Rust (`crate::api::set_compute_backend`, cxx,
/// nanobind, la UDF de Excel), así que vive junto al enum en vez de en cada frontera.
pub fn parse_backend_name(name: &str) -> Option<ComputeBackend> {
    match name.trim().to_ascii_lowercase().as_str() {
        "cpu" => Some(ComputeBackend::Cpu),
        "gpu" => Some(ComputeBackend::Gpu),
        _ => None,
    }
}

// Estado global de proceso, no thread-local ni por-llamada: PLAN.md §7.12 trata el backend
// como una opción de sesión de cálculo ("¿con qué calculo a partir de ahora?"), igual que
// `decimal.localcontext()`/`torch.device()` en Python tratan la precisión/el dispositivo por
// defecto como contexto ambiente en vez de un parámetro que hay que repetir en cada llamada.
// `AtomicU8` (no un `Mutex`) porque el propio valor cabe en una operación atómica y no hay
// invariante compuesta que proteger entre el `load` y el `store`.
static CURRENT: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);

/// Backend de cómputo actualmente seleccionado (por defecto `Cpu`).
pub fn current() -> ComputeBackend {
    ComputeBackend::from_code(CURRENT.load(std::sync::atomic::Ordering::SeqCst))
}

/// Cambia el backend actual. Devuelve `false` (sin cambiar nada) si se pide un backend no
/// disponible en este build (`ComputeBackend::is_available`) — así el llamador puede avisar
/// en vez de fallar en silencio calculando en CPU sin que nadie lo note.
pub fn set_current(backend: ComputeBackend) -> bool {
    if !backend.is_available() {
        return false;
    }
    CURRENT.store(backend.code(), std::sync::atomic::Ordering::SeqCst);
    true
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
    fn set_current_rejects_unavailable_backend_without_changing_state() {
        let before = current();
        let requested = if cfg!(feature = "gpu") {
            // Con la feature compilada, Gpu sí está disponible: no hay backend "no
            // disponible" que probar aquí salvo simular el caso por separado (ver el otro
            // branch). No tiene sentido este test con `gpu` activo; se deja un no-op.
            return;
        } else {
            ComputeBackend::Gpu
        };
        assert!(!set_current(requested));
        assert_eq!(current(), before);
    }
}
