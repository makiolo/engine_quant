//! Funciones en `f64` puro que expone el registry de la capa C++ (Fase 2, PLAN.md §5.4)
//! sobre el core Rust, sin tipos de Burn en la firma — misma frontera aislada que documenta
//! `crate::smoke`, pero esta ya es la API real: la superficie que `engine-ffi` bridgea al
//! `IMeasure` de la capa C++ (`ExposureProfileMeasure`/`UnilateralCvaMeasure`/
//! `PresentValueMeasure`/`Dv01Measure`), no un smoke test del pipeline de build.
//!
//! **Selección de backend (PLAN.md §7.15)**: `irs_hull_white_exposure_profile`/
//! `unilateral_cva_from_exposure` reciben `backend: &str` ("cpu"/"gpu") como **parámetro
//! explícito** de cada llamada — no leen ningún estado global. La resolución del nombre a un
//! `ComputeBackend` real es defensiva (`resolve_backend`, más abajo): un nombre inválido o no
//! compilado en este build cae a `Cpu` en silencio, porque la validación "de verdad" (rechazar
//! con un error claro) ya ocurrió antes, en `engine::ExecutionContext` (capa C++) — el mismo
//! valor validado allí es el que llega aquí.

use crate::backend::{self, ComputeBackend, CpuBackend};
use crate::backend::Autodiff;
use crate::exposure::{
    expected_exposure_profile, expected_exposure_profile_2f, expected_exposure_profile_2f_batch,
    expected_exposure_profile_batch, unilateral_cva, unilateral_cva_2f, unilateral_cva_2f_batch,
    unilateral_cva_batch, ExposureProfile,
};
use crate::models::hull_white::HullWhite1F;
use crate::models::hull_white_2f::HullWhite2F;
use crate::products::irs::IrSwap;
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

fn scalar<B: Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Traduce un nombre de backend ("cpu"/"gpu") a `ComputeBackend`, cayendo a `Cpu` si el
/// nombre no se reconoce o pide un backend no compilado en este build — ver documentación
/// del módulo: la validación estricta vive en `engine::ExecutionContext` (capa C++), no aquí.
fn resolve_backend(name: &str) -> ComputeBackend {
    backend::parse_backend_name(name)
        .filter(|b| b.is_available())
        .unwrap_or(ComputeBackend::Cpu)
}

/// Construye el IRS del caso base (PLAN.md §5.2) para un `model` ya construido: si
/// `use_par_rate`, el tipo fijo se calcula a la par en `start` a partir del `r0` **plano**
/// (`scalar(r0, device)`, sin `require_grad`) — deliberado: el cupón fijo de un swap ya
/// emitido no cambia cuando se shockea `r0` para medir una sensibilidad (`irs_hull_white_
/// npv_delta_r0`, más abajo), solo cambia el descuento/proyección con el que se revalora.
#[allow(clippy::too_many_arguments)]
fn build_irs_swap<B: Backend>(
    device: &burn::tensor::Device<B>,
    model: &HullWhite1F<B>,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
) -> IrSwap<B> {
    let rate = if use_par_rate {
        IrSwap::par_rate(scalar(r0, device), start, payment_times, accruals, model)
    } else {
        scalar(fixed_rate, device)
    };

    IrSwap {
        notional: scalar(notional, device),
        fixed_rate: rate,
        start,
        payment_times: payment_times.to_vec(),
        accruals: accruals.to_vec(),
    }
}

/// Construye el IRS de un LOTE homogéneo (PLAN.md §7.19): `notionals`/`fixed_rates` son
/// columnas (`[n_trades]`), sin `use_par_rate` -- cada trade del lote debe traer su
/// `fixed_rate` explícito (mismo motivo ya documentado en `irs_hull_white_npv_batch`: el
/// primitivo de lote no calcula "a la par" por trade). Compartido por las versiones de lote de
/// PV/DV01/ExpectedExposure/PFE95/UnilateralCVA de ambos modelos -- construir el `IrSwap` no
/// depende de qué modelo se use, solo `zero_coupon_bond` lo hace.
fn build_irs_swap_batch<B: Backend>(
    device: &burn::tensor::Device<B>,
    notionals: &[f64],
    fixed_rates: &[f64],
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> IrSwap<B> {
    assert_eq!(
        notionals.len(),
        fixed_rates.len(),
        "el lote requiere un fixed_rate por notional"
    );
    IrSwap {
        notional: Tensor::from_data(TensorData::from(notionals), device),
        fixed_rate: Tensor::from_data(TensorData::from(fixed_rates), device),
        start,
        payment_times,
        accruals,
    }
}

#[allow(clippy::too_many_arguments)]
fn exposure_profile_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> ExposureProfile {
    let model: HullWhite1F<B> = HullWhite1F::new(scalar(a, device), scalar(b, device), scalar(sigma, device));
    let swap = build_irs_swap(device, &model, r0, notional, fixed_rate, use_par_rate, start, payment_times, accruals);

    expected_exposure_profile(&model, &swap, r0, monitoring_times, n_steps, n_paths, seed, device)
}

/// Perfil de exposición (EE/PFE) de un IRS arbitrario bajo Hull-White 1F (PLAN.md §5.2),
/// generalización de `crate::smoke::irs_unilateral_cva_5y` (que fija el IRS a 5y anual):
/// aquí `payment_times`/`accruals` son arbitrarios y el resultado expone el perfil completo,
/// no solo el CVA agregado, para que la capa C++ (`measure.hpp`) pueda componer medidas.
/// Corre en `CpuBackend` o `GpuBackend` según `backend` (ver docs del módulo).
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_exposure_profile(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> ExposureProfile {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            exposure_profile_on::<CpuBackend>(
                &device, a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
                &payment_times, &accruals, monitoring_times, n_steps, n_paths, seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                exposure_profile_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
                    &payment_times, &accruals, monitoring_times, n_steps, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                // Inalcanzable en la práctica: `resolve_backend` ya cae a `Cpu` cuando este
                // build no tiene la feature `gpu`. CPU como red de seguridad, no como
                // comportamiento normal.
                let device = burn::tensor::Device::<CpuBackend>::default();
                exposure_profile_on::<CpuBackend>(
                    &device, a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
                    &payment_times, &accruals, monitoring_times, n_steps, n_paths, seed,
                )
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn exposure_profile_batch_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notionals: &[f64],
    fixed_rates: &[f64],
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> Vec<ExposureProfile> {
    let model: HullWhite1F<B> = HullWhite1F::new(scalar(a, device), scalar(b, device), scalar(sigma, device));
    let swap = build_irs_swap_batch(device, notionals, fixed_rates, start, payment_times, accruals);

    expected_exposure_profile_batch(&model, &swap, r0, monitoring_times, n_steps, n_paths, seed, device)
}

/// Equivalente de lote de `irs_hull_white_exposure_profile` (PLAN.md §7.19, "homogeneous
/// batch" de §7.17): simula el tipo corto **una sola vez** (todos los trades comparten
/// escenario) y devuelve un `ExposureProfile` por trade, mismo orden que `notionals`/
/// `fixed_rates`. Restricciones heredadas del lote (ver `build_irs_swap_batch`): mismo
/// calendario para todos los trades, sin `use_par_rate`.
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_exposure_profile_batch(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> Vec<ExposureProfile> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            exposure_profile_batch_on::<CpuBackend>(
                &device, a, b, sigma, r0, &notionals, &fixed_rates, start, payment_times, accruals,
                monitoring_times, n_steps, n_paths, seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                exposure_profile_batch_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, r0, &notionals, &fixed_rates, start, payment_times, accruals,
                    monitoring_times, n_steps, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                exposure_profile_batch_on::<CpuBackend>(
                    &device, a, b, sigma, r0, &notionals, &fixed_rates, start, payment_times, accruals,
                    monitoring_times, n_steps, n_paths, seed,
                )
            }
        }
    }
}

fn unilateral_cva_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    let model: HullWhite1F<B> = HullWhite1F::new(scalar(a, device), scalar(b, device), scalar(sigma, device));
    let profile = ExposureProfile {
        times,
        ee,
        pfe_95: Vec::new(),
    };
    unilateral_cva(&profile, &model, r0, hazard_rate, recovery_rate, device)
}

/// CVA unilateral a partir de un perfil de exposición ya calculado (`times`/`ee`, mismo
/// largo) — separa el cálculo del perfil del cálculo del CVA para que la capa C++ pueda
/// componerlas sin recalcular la simulación Monte Carlo. `backend` como en
/// `irs_hull_white_exposure_profile` (ver docs del módulo).
pub fn unilateral_cva_from_exposure(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            unilateral_cva_on::<CpuBackend>(&device, a, b, sigma, r0, times, ee, hazard_rate, recovery_rate)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                unilateral_cva_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, r0, times, ee, hazard_rate, recovery_rate,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                unilateral_cva_on::<CpuBackend>(&device, a, b, sigma, r0, times, ee, hazard_rate, recovery_rate)
            }
        }
    }
}

fn unilateral_cva_batch_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    profiles: Vec<ExposureProfile>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> Vec<f64> {
    let model: HullWhite1F<B> = HullWhite1F::new(scalar(a, device), scalar(b, device), scalar(sigma, device));
    unilateral_cva_batch(&profiles, &model, r0, hazard_rate, recovery_rate, device)
}

/// Equivalente de lote de `unilateral_cva_from_exposure`: un CVA por perfil de `profiles`
/// (mismo orden que devuelve `irs_hull_white_exposure_profile_batch`). `profiles` es
/// `Vec<ExposureProfile>` en vez de `times`/`ee` planos porque un `Vec<Vec<f64>>` no cruza la
/// frontera `cxx` (PLAN.md §5.5) -- `engine-ffi` convierte campo a campo desde
/// `Vec<ffi::ExposureProfileResult>`, el mismo struct que ya devuelve el batch de exposición.
/// `hazard_rate`/`recovery_rate` son compartidos por todo el lote (vienen del mismo
/// `MarketSnapshot` en la capa C++).
pub fn unilateral_cva_from_exposure_batch(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    profiles: Vec<ExposureProfile>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> Vec<f64> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            unilateral_cva_batch_on::<CpuBackend>(&device, a, b, sigma, r0, profiles, hazard_rate, recovery_rate)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                unilateral_cva_batch_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, r0, profiles, hazard_rate, recovery_rate,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                unilateral_cva_batch_on::<CpuBackend>(&device, a, b, sigma, r0, profiles, hazard_rate, recovery_rate)
            }
        }
    }
}

/// `true` si este build se compiló con soporte GPU (feature `gpu` de `engine-core`),
/// independientemente de cuál sea el backend seleccionado ahora mismo — permite a los
/// clientes distinguir "no lo has activado" de "no está disponible en este build".
pub fn is_gpu_backend_available() -> bool {
    ComputeBackend::Gpu.is_available()
}

/// NPV determinista (sin Monte Carlo) del IRS a `t=0` bajo Hull-White 1F — la medida "PV"
/// de `ENGINE.CALC` (PLAN.md §7.15, `PresentValueMeasure`). Siempre en `CpuBackend`: una
/// única evaluación no se beneficia de GPU (eso es cosa de `irs_hull_white_exposure_profile`,
/// que vectoriza sobre paths), así que esta función no toma `backend` como parámetro.
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_npv(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> f64 {
    let device = burn::tensor::Device::<CpuBackend>::default();
    let model: HullWhite1F<CpuBackend> = HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let swap = build_irs_swap(&device, &model, r0, notional, fixed_rate, use_par_rate, start, &payment_times, &accruals);
    swap.npv(scalar(r0, &device), 0.0, &model).into_scalar()
}

/// `d(NPV)/d(r0)` del IRS a `t=0` vía autodiff en modo reverse (PLAN.md §5.3, mismo patrón
/// que `crate::smoke::hull_white_zero_coupon_bond_delta_r0` pero sobre el NPV del swap
/// completo, no solo un bono cero-cupón) — la sensibilidad cruda detrás de la medida "DV01"
/// de `ENGINE.CALC` (`Dv01Measure` multiplica esto por 0.0001 en la capa C++, no aquí).
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_npv_delta_r0(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> f64 {
    type AD = Autodiff<CpuBackend>;
    let device = burn::tensor::Device::<AD>::default();
    let model: HullWhite1F<AD> = HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let swap = build_irs_swap(&device, &model, r0, notional, fixed_rate, use_par_rate, start, &payment_times, &accruals);

    let r0_var: Tensor<AD, 1> = scalar(r0, &device).require_grad();
    let price = swap.npv(r0_var.clone(), 0.0, &model);
    let grads = price.backward();
    r0_var.grad(&grads).unwrap().into_scalar()
}

/// NPV de un **lote homogéneo** de IRS bajo Hull-White 1F (PLAN.md §7.17, nivel "homogeneous
/// batch" de la API de cálculo): `notionals.len()` swaps que comparten calendario
/// (`start`/`payment_times`/`accruals`) y estado de mercado (`a`/`b`/`sigma`/`r0`), pero
/// difieren en `notional`/`fixed_rate`, valorados con una única llamada a `IrSwap::npv` — sin
/// bucle escalar en Rust. No es código nuevo de valoración: `IrSwap::npv` ya difundía
/// `notional`/`fixed_rate` de forma `[1]` contra el estado de forma `[n_paths]` (ver docs de
/// `products::irs`); aquí se aprovecha exactamente el mismo mecanismo de Burn dándoles forma
/// `[n_swaps]` en vez de `[1]`, que se difunde igual contra el estado (forma `[1]`, un único
/// `r0`). `use_par_rate` no aplica a un lote (cada swap ya trae su propio `fixed_rate`) — a
/// diferencia de `irs_hull_white_npv`, que sí ofrece ese atajo para un swap suelto.
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_npv_batch(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    assert_eq!(
        notionals.len(),
        fixed_rates.len(),
        "irs_hull_white_npv_batch requiere un fixed_rate por notional"
    );
    let device = burn::tensor::Device::<CpuBackend>::default();
    let model: HullWhite1F<CpuBackend> = HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let swap = IrSwap {
        notional: Tensor::from_data(TensorData::from(notionals.as_slice()), &device),
        fixed_rate: Tensor::from_data(TensorData::from(fixed_rates.as_slice()), &device),
        start,
        payment_times,
        accruals,
    };
    swap.npv(scalar(r0, &device), 0.0, &model)
        .into_data()
        .to_vec::<f64>()
        .unwrap()
}

/// Equivalente de lote de `irs_hull_white_npv_delta_r0`: un `d(NPV)/d(r0)` por trade. **No**
/// es una sola pasada `backward()` para todo el lote -- con `r0` compartido y N salidas
/// independientes, una única `backward()` sobre la suma de NPVs del lote solo da la *suma* de
/// las N sensibilidades (reverse-mode AD reduce el cotangente en la dirección en la que se
/// difundió `r0`), no cada una por separado; conseguir cada delta por trade exige N pasadas
/// backward, una por trade. El lote evita N *round-trips* de FFI/C++/Python/Excel -- no evita
/// las N pasadas backward en sí, que siguen siendo O(n_trades) aquí dentro de Rust (PLAN.md
/// §7.19 lo documenta explícitamente: no es el mismo tipo de ahorro que PV/ExpectedExposure).
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_npv_delta_r0_batch(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    assert_eq!(
        notionals.len(),
        fixed_rates.len(),
        "el lote requiere un fixed_rate por notional"
    );
    notionals
        .iter()
        .zip(fixed_rates.iter())
        .map(|(&notional, &fixed_rate)| {
            irs_hull_white_npv_delta_r0(
                a, b, sigma, r0, notional, fixed_rate, false, start, payment_times.clone(), accruals.clone(),
            )
        })
        .collect()
}

/// Construye el IRS del caso base bajo `HullWhite2F` (PLAN.md §7.16), mismo rol que
/// `build_irs_swap` para `HullWhite1F`: si `use_par_rate`, el tipo fijo se calcula a la par
/// en `start` a partir del estado inicial `(0, 0)` -- a diferencia de `HullWhite1F`, aquí no
/// hace falta un `r0` aparte para ese estado: el nivel de tipos ya vive en `model.r0`
/// (`phi0`), ver `crate::models::hull_white_2f`.
#[allow(clippy::too_many_arguments)]
fn build_irs_swap_2f<B: Backend>(
    device: &burn::tensor::Device<B>,
    model: &HullWhite2F<B>,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
) -> IrSwap<B> {
    let state0 = (scalar(0.0, device), scalar(0.0, device));
    let rate = if use_par_rate {
        IrSwap::par_rate(state0, start, payment_times, accruals, model)
    } else {
        scalar(fixed_rate, device)
    };

    IrSwap {
        notional: scalar(notional, device),
        fixed_rate: rate,
        start,
        payment_times: payment_times.to_vec(),
        accruals: accruals.to_vec(),
    }
}

#[allow(clippy::too_many_arguments)]
fn exposure_profile_2f_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> ExposureProfile {
    let model: HullWhite2F<B> =
        HullWhite2F::new(scalar(a, device), scalar(b, device), scalar(sigma, device), scalar(eta, device), rho, scalar(r0, device));
    let swap = build_irs_swap_2f(device, &model, notional, fixed_rate, use_par_rate, start, payment_times, accruals);

    expected_exposure_profile_2f(&model, &swap, monitoring_times, n_steps, n_paths, seed, device)
}

/// Perfil de exposición (EE/PFE) de un IRS arbitrario bajo Hull-White 2 factores (PLAN.md
/// §7.16), segundo modelo del motor junto a `irs_hull_white_exposure_profile` -- misma
/// frontera `f64` pura, misma selección de `backend` explícita, mismo shape de resultado
/// (`ExposureProfile`), de forma que la capa C++ (`engine/measure.hpp`) pueda tratar ambos
/// modelos con el mismo código de medida (PLAN.md §7.16: "mismo interfaz homogéneo").
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_2f_exposure_profile(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> ExposureProfile {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            exposure_profile_2f_on::<CpuBackend>(
                &device, a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start,
                &payment_times, &accruals, monitoring_times, n_steps, n_paths, seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                exposure_profile_2f_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start,
                    &payment_times, &accruals, monitoring_times, n_steps, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                exposure_profile_2f_on::<CpuBackend>(
                    &device, a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start,
                    &payment_times, &accruals, monitoring_times, n_steps, n_paths, seed,
                )
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn exposure_profile_2f_batch_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notionals: &[f64],
    fixed_rates: &[f64],
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> Vec<ExposureProfile> {
    let model: HullWhite2F<B> =
        HullWhite2F::new(scalar(a, device), scalar(b, device), scalar(sigma, device), scalar(eta, device), rho, scalar(r0, device));
    let swap = build_irs_swap_batch(device, notionals, fixed_rates, start, payment_times, accruals);

    expected_exposure_profile_2f_batch(&model, &swap, monitoring_times, n_steps, n_paths, seed, device)
}

/// Equivalente de lote de `irs_hull_white_2f_exposure_profile` -- ver
/// `irs_hull_white_exposure_profile_batch` (misma restricción de calendario compartido, sin
/// `use_par_rate`).
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_2f_exposure_profile_batch(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
) -> Vec<ExposureProfile> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            exposure_profile_2f_batch_on::<CpuBackend>(
                &device, a, b, sigma, eta, rho, r0, &notionals, &fixed_rates, start, payment_times, accruals,
                monitoring_times, n_steps, n_paths, seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                exposure_profile_2f_batch_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, eta, rho, r0, &notionals, &fixed_rates, start, payment_times, accruals,
                    monitoring_times, n_steps, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                exposure_profile_2f_batch_on::<CpuBackend>(
                    &device, a, b, sigma, eta, rho, r0, &notionals, &fixed_rates, start, payment_times, accruals,
                    monitoring_times, n_steps, n_paths, seed,
                )
            }
        }
    }
}

fn unilateral_cva_2f_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    let model: HullWhite2F<B> =
        HullWhite2F::new(scalar(a, device), scalar(b, device), scalar(sigma, device), scalar(eta, device), rho, scalar(r0, device));
    let profile = ExposureProfile {
        times,
        ee,
        pfe_95: Vec::new(),
    };
    unilateral_cva_2f(&profile, &model, hazard_rate, recovery_rate, device)
}

/// CVA unilateral bajo Hull-White 2 factores a partir de un perfil ya calculado -- mismo rol
/// que `unilateral_cva_from_exposure` para `HullWhite1F` (PLAN.md §7.16).
#[allow(clippy::too_many_arguments)]
pub fn unilateral_cva_from_exposure_2f(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            unilateral_cva_2f_on::<CpuBackend>(&device, a, b, sigma, eta, rho, r0, times, ee, hazard_rate, recovery_rate)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                unilateral_cva_2f_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, eta, rho, r0, times, ee, hazard_rate, recovery_rate,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                unilateral_cva_2f_on::<CpuBackend>(&device, a, b, sigma, eta, rho, r0, times, ee, hazard_rate, recovery_rate)
            }
        }
    }
}

fn unilateral_cva_2f_batch_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    profiles: Vec<ExposureProfile>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> Vec<f64> {
    let model: HullWhite2F<B> =
        HullWhite2F::new(scalar(a, device), scalar(b, device), scalar(sigma, device), scalar(eta, device), rho, scalar(r0, device));
    unilateral_cva_2f_batch(&profiles, &model, hazard_rate, recovery_rate, device)
}

/// Equivalente de lote de `unilateral_cva_from_exposure_2f` -- ver
/// `unilateral_cva_from_exposure_batch` (mismo motivo para recibir `Vec<ExposureProfile>` en
/// vez de `times`/`ee` planos: un `Vec<Vec<f64>>` no cruza `cxx`).
#[allow(clippy::too_many_arguments)]
pub fn unilateral_cva_from_exposure_2f_batch(
    backend: &str,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    profiles: Vec<ExposureProfile>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> Vec<f64> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            unilateral_cva_2f_batch_on::<CpuBackend>(&device, a, b, sigma, eta, rho, r0, profiles, hazard_rate, recovery_rate)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                unilateral_cva_2f_batch_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, eta, rho, r0, profiles, hazard_rate, recovery_rate,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                unilateral_cva_2f_batch_on::<CpuBackend>(&device, a, b, sigma, eta, rho, r0, profiles, hazard_rate, recovery_rate)
            }
        }
    }
}

/// NPV determinista del IRS a `t=0` bajo Hull-White 2 factores -- mismo rol que
/// `irs_hull_white_npv`, siempre en `CpuBackend` por el mismo motivo (PLAN.md §7.16).
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_2f_npv(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> f64 {
    let device = burn::tensor::Device::<CpuBackend>::default();
    let model: HullWhite2F<CpuBackend> =
        HullWhite2F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device), scalar(eta, &device), rho, scalar(r0, &device));
    let swap = build_irs_swap_2f(&device, &model, notional, fixed_rate, use_par_rate, start, &payment_times, &accruals);
    let state0 = (scalar(0.0, &device), scalar(0.0, &device));
    swap.npv(state0, 0.0, &model).into_scalar()
}

/// Equivalente de lote de `irs_hull_white_2f_npv` -- mismo mecanismo de broadcasting "gratis"
/// que `irs_hull_white_npv_batch` (PLAN.md §7.17/§7.19): `notionals`/`fixed_rates` forma
/// `[n_trades]`, el estado `(0,0)` sigue forma `[1]`.
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_2f_npv_batch(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    let device = burn::tensor::Device::<CpuBackend>::default();
    let model: HullWhite2F<CpuBackend> =
        HullWhite2F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device), scalar(eta, &device), rho, scalar(r0, &device));
    let swap = build_irs_swap_batch(&device, &notionals, &fixed_rates, start, payment_times, accruals);
    let state0 = (scalar(0.0, &device), scalar(0.0, &device));
    swap.npv(state0, 0.0, &model).into_data().to_vec::<f64>().unwrap()
}

/// `d(NPV)/d(r0)` del IRS bajo Hull-White 2 factores vía autodiff -- mismo rol que
/// `irs_hull_white_npv_delta_r0`. A diferencia del modelo de 1 factor (donde `r0` es el
/// *estado* que se pasa a `IrSwap::npv`), aquí `r0` es un parámetro del propio modelo
/// (`HullWhite2F::r0`, el desplazamiento `phi0`): se usan dos instancias del modelo, una
/// "plana" (sin gradiente) para construir el swap -- el cupón fijo no debe depender de un
/// shock instantáneo a `r0`, mismo motivo documentado en `build_irs_swap` -- y otra con
/// `r0.require_grad()` solo para el NPV cuya sensibilidad se pide.
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_2f_npv_delta_r0(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> f64 {
    type AD = Autodiff<CpuBackend>;
    let device = burn::tensor::Device::<AD>::default();

    let plain_model: HullWhite2F<AD> =
        HullWhite2F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device), scalar(eta, &device), rho, scalar(r0, &device));
    let swap = build_irs_swap_2f(&device, &plain_model, notional, fixed_rate, use_par_rate, start, &payment_times, &accruals);

    let r0_var: Tensor<AD, 1> = scalar(r0, &device).require_grad();
    let diff_model: HullWhite2F<AD> =
        HullWhite2F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device), scalar(eta, &device), rho, r0_var.clone());

    let state0 = (scalar(0.0, &device), scalar(0.0, &device));
    let price = swap.npv(state0, 0.0, &diff_model);
    let grads = price.backward();
    r0_var.grad(&grads).unwrap().into_scalar()
}

/// Equivalente de lote de `irs_hull_white_2f_npv_delta_r0` -- ver
/// `irs_hull_white_npv_delta_r0_batch` para el porqué de las N pasadas backward (una por
/// trade, no una sola para todo el lote).
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_2f_npv_delta_r0_batch(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    assert_eq!(
        notionals.len(),
        fixed_rates.len(),
        "el lote requiere un fixed_rate por notional"
    );
    notionals
        .iter()
        .zip(fixed_rates.iter())
        .map(|(&notional, &fixed_rate)| {
            irs_hull_white_2f_npv_delta_r0(
                a, b, sigma, eta, rho, r0, notional, fixed_rate, false, start, payment_times.clone(), accruals.clone(),
            )
        })
        .collect()
}

/// Calibra `a`/`b` de `HullWhite1F` a una curva de mercado (`pillars`/`zero_rates`, mismo
/// largo, `pillars` estrictamente creciente) partiendo de `(initial_a, initial_b)`; `sigma`/
/// `r0` no se calibran, ver `crate::calibration` para el porqué. Traduce
/// `crate::market::MarketSnapshot` (que ya es `f64` puro) a esta frontera solo para mantener
/// la misma convención que el resto de `crate::api`: un único punto por el que `engine-ffi`
/// entra al core.
pub fn calibrate_hull_white(
    pillars: Vec<f64>,
    zero_rates: Vec<f64>,
    initial_a: f64,
    initial_b: f64,
    sigma: f64,
    r0: f64,
) -> crate::calibration::HullWhiteCalibrationResult {
    let market = crate::market::MarketSnapshot::new(pillars, zero_rates);
    crate::calibration::calibrate_hull_white(&market, initial_a, initial_b, sigma, r0)
}

/// Equivalente de dos factores de `calibrate_hull_white` (PLAN.md §7.18): calibra `a`/`b` de
/// `HullWhite2F` a una curva de mercado, ver `crate::calibration` para el porqué solo esos dos
/// parámetros (de los seis del modelo) se calibran.
#[allow(clippy::too_many_arguments)]
pub fn calibrate_hull_white_2f(
    pillars: Vec<f64>,
    zero_rates: Vec<f64>,
    initial_a: f64,
    initial_b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
) -> crate::calibration::HullWhite2FCalibrationResult {
    let market = crate::market::MarketSnapshot::new(pillars, zero_rates);
    crate::calibration::calibrate_hull_white_2f(&market, initial_a, initial_b, sigma, eta, rho, r0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exposure_profile_is_nonnegative_and_pfe_dominates_ee() {
        // PLAN.md §7.19: ver `crate::rng_test_lock` -- `B::seed` (burn-ndarray) es un Mutex
        // global de proceso, no por hilo.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let profile = irs_hull_white_exposure_profile(
            "cpu",
            0.1,
            0.03,
            0.01,
            0.02,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0],
            vec![1.0; 5],
            &[0.0, 1.0, 2.0],
            104,
            5_000,
            7,
        );
        for i in 0..profile.times.len() {
            assert!(profile.ee[i] >= 0.0);
            assert!(profile.pfe_95[i] >= profile.ee[i]);
        }
    }

    #[test]
    fn exposure_profile_rejects_unknown_backend_by_falling_back_to_cpu() {
        // resolve_backend cae a Cpu para un nombre desconocido -- la validacion estricta
        // vive en engine::ExecutionContext (capa C++), no aqui (ver docs del modulo).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let profile = irs_hull_white_exposure_profile(
            "tpu",
            0.1,
            0.03,
            0.01,
            0.02,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0],
            vec![1.0; 5],
            &[0.0, 1.0, 2.0],
            104,
            1_000,
            7,
        );
        assert!(profile.ee[0] >= 0.0);
    }

    #[test]
    fn cva_from_exposure_is_nonnegative() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let profile = irs_hull_white_exposure_profile(
            "cpu",
            0.1,
            0.03,
            0.01,
            0.02,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0],
            vec![1.0; 5],
            &[0.0, 1.0, 2.0, 3.0],
            156,
            5_000,
            13,
        );
        let cva = unilateral_cva_from_exposure(
            "cpu", 0.1, 0.03, 0.01, 0.02, profile.times, profile.ee, 0.02, 0.4,
        );
        assert!(cva >= 0.0);
    }

    #[test]
    fn cva_from_exposure_is_zero_when_hazard_rate_is_zero() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let profile = irs_hull_white_exposure_profile(
            "cpu",
            0.1,
            0.03,
            0.01,
            0.02,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0],
            vec![1.0; 5],
            &[0.0, 1.0, 2.0],
            104,
            2_000,
            17,
        );
        let cva = unilateral_cva_from_exposure(
            "cpu", 0.1, 0.03, 0.01, 0.02, profile.times, profile.ee, 0.0, 0.4,
        );
        assert!(cva.abs() < 1e-9, "CVA con hazard=0 debería ser 0, got {cva}");
    }

    #[test]
    fn irs_hull_white_npv_of_a_par_swap_is_near_zero() {
        // Un swap "a la par" (fixed_rate calculado para NPV=0 en start) debe dar NPV~0 al
        // valorarlo en ese mismo punto -- mismo caso que products::irs::tests::par_swap_has_
        // zero_npv_at_start, aquí a través de la frontera f64 pura de crate::api.
        let npv = irs_hull_white_npv(
            0.1, 0.03, 0.01, 0.02, 1_000_000.0, 0.0, true, 0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0], vec![1.0; 5],
        );
        assert!(npv.abs() < 1e-6, "NPV del swap a la par debería ser ~0, got {npv}");
    }

    #[test]
    fn irs_hull_white_npv_batch_matches_a_loop_of_scalar_calls() {
        // Nivel "homogeneous batch" (PLAN.md §7.17): el resultado vectorizado sobre 3 swaps
        // que comparten calendario debe coincidir, swap a swap, con llamar a
        // irs_hull_white_npv (escalar) una vez por swap -- prueba de que difundir
        // notional/fixed_rate de forma [n_swaps] no cambia la valoración de cada uno.
        let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
        let start = 0.0;
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let notionals = vec![1_000_000.0, 2_500_000.0, 500_000.0];
        let fixed_rates = vec![0.02, 0.015, 0.025];

        let batch = irs_hull_white_npv_batch(
            a, b, sigma, r0, notionals.clone(), fixed_rates.clone(), start,
            payment_times.clone(), accruals.clone(),
        );
        assert_eq!(batch.len(), notionals.len());

        for i in 0..notionals.len() {
            let scalar_npv = irs_hull_white_npv(
                a, b, sigma, r0, notionals[i], fixed_rates[i], false, start,
                payment_times.clone(), accruals.clone(),
            );
            assert!(
                (batch[i] - scalar_npv).abs() < 1e-6,
                "swap {i}: batch={} escalar={scalar_npv}", batch[i]
            );
        }
    }

    #[test]
    fn irs_hull_white_npv_delta_r0_is_positive_for_a_payer_swap() {
        // Swap pagador (paga fijo, recibe flotante) a la par: sube de valor cuando suben los
        // tipos -- mismo hecho que products::irs::tests::payer_swap_npv_increases_when_rates_
        // rise, aquí como derivada continua en vez de bump discreto.
        let delta = irs_hull_white_npv_delta_r0(
            0.1, 0.03, 0.01, 0.02, 1_000_000.0, 0.0, true, 0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0], vec![1.0; 5],
        );
        assert!(delta > 0.0, "delta debería ser positivo, got {delta}");
    }

    #[test]
    fn irs_hull_white_npv_delta_r0_batch_matches_a_loop_of_scalar_calls() {
        let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
        let start = 0.0;
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let notionals = vec![1_000_000.0, 2_500_000.0, 500_000.0];
        let fixed_rates = vec![0.02, 0.015, 0.025];

        let batch = irs_hull_white_npv_delta_r0_batch(
            a, b, sigma, r0, notionals.clone(), fixed_rates.clone(), start,
            payment_times.clone(), accruals.clone(),
        );
        assert_eq!(batch.len(), notionals.len());

        for i in 0..notionals.len() {
            let scalar_delta = irs_hull_white_npv_delta_r0(
                a, b, sigma, r0, notionals[i], fixed_rates[i], false, start,
                payment_times.clone(), accruals.clone(),
            );
            assert!(
                (batch[i] - scalar_delta).abs() < 1e-9,
                "swap {i}: batch={} escalar={scalar_delta}", batch[i]
            );
        }
    }

    #[test]
    fn irs_hull_white_2f_npv_batch_matches_a_loop_of_scalar_calls() {
        let (a, b, sigma, eta, rho, r0) = (0.1, 0.2, 0.01, 0.012, -0.7, 0.03);
        let start = 0.0;
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let notionals = vec![1_000_000.0, 2_500_000.0, 500_000.0];
        let fixed_rates = vec![0.02, 0.015, 0.025];

        let batch = irs_hull_white_2f_npv_batch(
            a, b, sigma, eta, rho, r0, notionals.clone(), fixed_rates.clone(), start,
            payment_times.clone(), accruals.clone(),
        );
        assert_eq!(batch.len(), notionals.len());

        for i in 0..notionals.len() {
            let scalar_npv = irs_hull_white_2f_npv(
                a, b, sigma, eta, rho, r0, notionals[i], fixed_rates[i], false, start,
                payment_times.clone(), accruals.clone(),
            );
            assert!(
                (batch[i] - scalar_npv).abs() < 1e-6,
                "swap {i}: batch={} escalar={scalar_npv}", batch[i]
            );
        }
    }

    #[test]
    fn irs_hull_white_2f_npv_delta_r0_batch_matches_a_loop_of_scalar_calls() {
        let (a, b, sigma, eta, rho, r0) = (0.1, 0.2, 0.01, 0.012, -0.7, 0.03);
        let start = 0.0;
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let notionals = vec![1_000_000.0, 2_500_000.0, 500_000.0];
        let fixed_rates = vec![0.02, 0.015, 0.025];

        let batch = irs_hull_white_2f_npv_delta_r0_batch(
            a, b, sigma, eta, rho, r0, notionals.clone(), fixed_rates.clone(), start,
            payment_times.clone(), accruals.clone(),
        );
        assert_eq!(batch.len(), notionals.len());

        for i in 0..notionals.len() {
            let scalar_delta = irs_hull_white_2f_npv_delta_r0(
                a, b, sigma, eta, rho, r0, notionals[i], fixed_rates[i], false, start,
                payment_times.clone(), accruals.clone(),
            );
            assert!(
                (batch[i] - scalar_delta).abs() < 1e-9,
                "swap {i}: batch={} escalar={scalar_delta}", batch[i]
            );
        }
    }

    #[test]
    fn irs_hull_white_exposure_profile_batch_matches_a_loop_of_scalar_calls() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
        let start = 0.0;
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let notionals = vec![1_000_000.0, 2_500_000.0];
        let fixed_rates = vec![0.02, 0.015];
        let times = [0.0, 1.0, 2.0];
        let (n_steps, n_paths, seed) = (104, 2_000, 7);

        let batch = irs_hull_white_exposure_profile_batch(
            "cpu", a, b, sigma, r0, notionals.clone(), fixed_rates.clone(), start,
            payment_times.clone(), accruals.clone(), &times, n_steps, n_paths, seed,
        );
        assert_eq!(batch.len(), notionals.len());

        let cva_batch = unilateral_cva_from_exposure_batch(
            "cpu", a, b, sigma, r0, batch.clone(), 0.02, 0.4,
        );
        assert_eq!(cva_batch.len(), notionals.len());

        for i in 0..notionals.len() {
            let scalar_profile = irs_hull_white_exposure_profile(
                "cpu", a, b, sigma, r0, notionals[i], fixed_rates[i], false, start,
                payment_times.clone(), accruals.clone(), &times, n_steps, n_paths, seed,
            );
            for k in 0..times.len() {
                assert!(
                    (batch[i].ee[k] - scalar_profile.ee[k]).abs() < 1e-9,
                    "swap {i} fecha {k}: EE batch={} escalar={}", batch[i].ee[k], scalar_profile.ee[k]
                );
                assert!(
                    (batch[i].pfe_95[k] - scalar_profile.pfe_95[k]).abs() < 1e-9,
                    "swap {i} fecha {k}: PFE95 batch={} escalar={}", batch[i].pfe_95[k], scalar_profile.pfe_95[k]
                );
            }
            let scalar_cva = unilateral_cva_from_exposure(
                "cpu", a, b, sigma, r0, scalar_profile.times, scalar_profile.ee, 0.02, 0.4,
            );
            assert!(
                (cva_batch[i] - scalar_cva).abs() < 1e-9,
                "swap {i}: CVA batch={} escalar={scalar_cva}", cva_batch[i]
            );
        }
    }

    #[test]
    fn irs_hull_white_2f_exposure_profile_batch_matches_a_loop_of_scalar_calls() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (a, b, sigma, eta, rho, r0) = (0.1, 0.2, 0.01, 0.012, -0.7, 0.03);
        let start = 0.0;
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let notionals = vec![1_000_000.0, 2_500_000.0];
        let fixed_rates = vec![0.02, 0.015];
        let times = [0.0, 1.0, 2.0];
        let (n_steps, n_paths, seed) = (104, 2_000, 7);

        let batch = irs_hull_white_2f_exposure_profile_batch(
            "cpu", a, b, sigma, eta, rho, r0, notionals.clone(), fixed_rates.clone(), start,
            payment_times.clone(), accruals.clone(), &times, n_steps, n_paths, seed,
        );
        assert_eq!(batch.len(), notionals.len());

        let cva_batch = unilateral_cva_from_exposure_2f_batch(
            "cpu", a, b, sigma, eta, rho, r0, batch.clone(), 0.02, 0.4,
        );
        assert_eq!(cva_batch.len(), notionals.len());

        for i in 0..notionals.len() {
            let scalar_profile = irs_hull_white_2f_exposure_profile(
                "cpu", a, b, sigma, eta, rho, r0, notionals[i], fixed_rates[i], false, start,
                payment_times.clone(), accruals.clone(), &times, n_steps, n_paths, seed,
            );
            for k in 0..times.len() {
                assert!(
                    (batch[i].ee[k] - scalar_profile.ee[k]).abs() < 1e-9,
                    "swap {i} fecha {k}: EE batch={} escalar={}", batch[i].ee[k], scalar_profile.ee[k]
                );
                assert!(
                    (batch[i].pfe_95[k] - scalar_profile.pfe_95[k]).abs() < 1e-9,
                    "swap {i} fecha {k}: PFE95 batch={} escalar={}", batch[i].pfe_95[k], scalar_profile.pfe_95[k]
                );
            }
            let scalar_cva = unilateral_cva_from_exposure_2f(
                "cpu", a, b, sigma, eta, rho, r0, scalar_profile.times, scalar_profile.ee, 0.02, 0.4,
            );
            assert!(
                (cva_batch[i] - scalar_cva).abs() < 1e-9,
                "swap {i}: CVA batch={} escalar={scalar_cva}", cva_batch[i]
            );
        }
    }

    #[test]
    fn exposure_profile_2f_is_nonnegative_and_pfe_dominates_ee() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let profile = irs_hull_white_2f_exposure_profile(
            "cpu",
            0.1,
            0.2,
            0.01,
            0.012,
            -0.7,
            0.03,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0],
            vec![1.0; 5],
            &[0.0, 1.0, 2.0],
            104,
            5_000,
            7,
        );
        for i in 0..profile.times.len() {
            assert!(profile.ee[i] >= 0.0);
            assert!(profile.pfe_95[i] >= profile.ee[i]);
        }
    }

    #[test]
    fn cva_from_exposure_2f_is_nonnegative() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let profile = irs_hull_white_2f_exposure_profile(
            "cpu",
            0.1,
            0.2,
            0.01,
            0.012,
            -0.7,
            0.03,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0],
            vec![1.0; 5],
            &[0.0, 1.0, 2.0, 3.0],
            156,
            5_000,
            13,
        );
        let cva = unilateral_cva_from_exposure_2f(
            "cpu", 0.1, 0.2, 0.01, 0.012, -0.7, 0.03, profile.times, profile.ee, 0.02, 0.4,
        );
        assert!(cva >= 0.0);
    }

    #[test]
    fn cva_from_exposure_2f_is_zero_when_hazard_rate_is_zero() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let profile = irs_hull_white_2f_exposure_profile(
            "cpu",
            0.1,
            0.2,
            0.01,
            0.012,
            -0.7,
            0.03,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0],
            vec![1.0; 5],
            &[0.0, 1.0, 2.0],
            104,
            2_000,
            17,
        );
        let cva = unilateral_cva_from_exposure_2f(
            "cpu", 0.1, 0.2, 0.01, 0.012, -0.7, 0.03, profile.times, profile.ee, 0.0, 0.4,
        );
        assert!(cva.abs() < 1e-9, "CVA con hazard=0 debería ser 0, got {cva}");
    }

    #[test]
    fn irs_hull_white_2f_npv_of_a_par_swap_is_near_zero() {
        let npv = irs_hull_white_2f_npv(
            0.1, 0.2, 0.01, 0.012, -0.7, 0.03, 1_000_000.0, 0.0, true, 0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0], vec![1.0; 5],
        );
        assert!(npv.abs() < 1e-6, "NPV del swap a la par debería ser ~0, got {npv}");
    }

    #[test]
    fn irs_hull_white_2f_npv_delta_r0_is_positive_for_a_payer_swap() {
        let delta = irs_hull_white_2f_npv_delta_r0(
            0.1, 0.2, 0.01, 0.012, -0.7, 0.03, 1_000_000.0, 0.0, true, 0.0,
            vec![1.0, 2.0, 3.0, 4.0, 5.0], vec![1.0; 5],
        );
        assert!(delta > 0.0, "delta debería ser positivo, got {delta}");
    }

    #[test]
    fn is_gpu_backend_available_matches_feature_flag() {
        assert_eq!(is_gpu_backend_available(), cfg!(feature = "gpu"));
    }

    #[test]
    fn calibrate_hull_white_recovers_known_parameters() {
        let (true_a, true_b, sigma, r0) = (0.15, 0.025, 0.008, 0.02);
        let pillars = vec![1.0, 2.0, 5.0, 10.0, 20.0];
        let market = crate::market::MarketSnapshot::synthetic_from_hull_white(true_a, true_b, sigma, r0, pillars.clone());

        let result = calibrate_hull_white(pillars, market.zero_rates().to_vec(), 0.3, 0.01, sigma, r0);

        assert!(result.converged, "no convergió: rmse={}", result.rmse);
        assert!((result.a - true_a).abs() < 1e-4);
        assert!((result.b - true_b).abs() < 1e-4);
    }

    #[test]
    fn calibrate_hull_white_2f_recovers_known_parameters() {
        let (true_a, true_b, sigma, eta, rho, r0) = (0.15, 0.25, 0.008, 0.01, -0.6, 0.02);
        let pillars = vec![1.0, 2.0, 5.0, 10.0, 20.0];
        let market =
            crate::market::MarketSnapshot::synthetic_from_hull_white_2f(true_a, true_b, sigma, eta, rho, r0, pillars.clone());

        let result = calibrate_hull_white_2f(pillars, market.zero_rates().to_vec(), 0.4, 0.05, sigma, eta, rho, r0);

        assert!(result.converged, "no convergió: rmse={}", result.rmse);
        assert!((result.a - true_a).abs() < 1e-4);
        assert!((result.b - true_b).abs() < 1e-4);
    }
}
