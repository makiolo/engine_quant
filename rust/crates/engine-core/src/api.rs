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

// Esta capa conserva firmas numéricas planas para la frontera C++/Python. Agruparlas
// cambiaría el contrato público que precisamente implementa este módulo.
#![allow(clippy::too_many_arguments)]

use crate::backend::{resolve_backend, ComputeBackend, CpuBackend};
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
/// de `ENGINE.PRICE` (PLAN.md §7.15, `PresentValueMeasure`). Siempre en `CpuBackend`: una
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
/// de `ENGINE.PRICE` (`Dv01Measure` multiplica esto por 0.0001 en la capa C++, no aquí).
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

/// Las cuatro derivadas de primer orden del NPV determinista de Hull-White 1F respecto de sus
/// cuatro parametros (PLAN_GREEKS.md §5.2/§11 Fase 7): resultado de `irs_hull_white_npv_all_greeks`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HullWhite1FGreeks {
    pub d_a: f64,
    pub d_b: f64,
    pub d_sigma: f64,
    pub d_r0: f64,
}

/// `d(NPV)/d(a)`, `d(NPV)/d(b)`, `d(NPV)/d(sigma)` y `d(NPV)/d(r0)` del IRS a `t=0` bajo
/// Hull-White 1F, TODAS en una unica pasada `backward()` (PLAN_GREEKS.md §5.2: "el coste de una
/// pasada backward() no depende de CUANTOS parametros se piden ... una sola llamada puede devolver
/// Delta+Rho+Vega+... de Hull-White de una vez"). Generaliza `irs_hull_white_npv_delta_r0`
/// marcando `a`/`b`/`sigma` como variables del grafo ademas de `r0`, en vez de constantes.
///
/// El swap (y, si `use_par_rate`, el tipo fijo a la par) se construye con un modelo "plano" (sin
/// gradiente, `a`/`b`/`sigma`/`r0` tal cual se recibieron) -- mismo motivo que documenta
/// `build_irs_swap`: el cupon fijo de un swap ya emitido no debe moverse cuando se shockea
/// CUALQUIERA de los cuatro parametros para medir una sensibilidad, solo el descuento/proyeccion
/// con el que se revalora debe depender del grafo diferenciable. El NPV que SI se diferencia se
/// recalcula sobre una segunda instancia del modelo (`diff_model`) cuyos cuatro tensores llevan
/// `require_grad()`.
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_npv_all_greeks(
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
) -> HullWhite1FGreeks {
    type AD = Autodiff<CpuBackend>;
    let device = burn::tensor::Device::<AD>::default();

    let plain_model: HullWhite1F<AD> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let swap = build_irs_swap(&device, &plain_model, r0, notional, fixed_rate, use_par_rate, start, &payment_times, &accruals);

    let a_var: Tensor<AD, 1> = scalar(a, &device).require_grad();
    let b_var: Tensor<AD, 1> = scalar(b, &device).require_grad();
    let sigma_var: Tensor<AD, 1> = scalar(sigma, &device).require_grad();
    let r0_var: Tensor<AD, 1> = scalar(r0, &device).require_grad();
    let diff_model: HullWhite1F<AD> = HullWhite1F::new(a_var.clone(), b_var.clone(), sigma_var.clone());

    let price = swap.npv(r0_var.clone(), 0.0, &diff_model);
    let grads = price.backward();
    HullWhite1FGreeks {
        d_a: a_var.grad(&grads).unwrap().into_scalar(),
        d_b: b_var.grad(&grads).unwrap().into_scalar(),
        d_sigma: sigma_var.grad(&grads).unwrap().into_scalar(),
        d_r0: r0_var.grad(&grads).unwrap().into_scalar(),
    }
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

/// Equivalente de `HullWhite1FGreeks` para Hull-White 2 factores (PLAN_GREEKS.md §11 Fase 7).
/// NO incluye `d_rho`: `HullWhite2F::new` recibe `rho` como `f64` PLANO (no como `Tensor`, ver
/// `models::hull_white_2f::HullWhite2F::new`), asi que no forma parte del grafo de autodiff -- no
/// hay gradiente reverse-mode que leer para `rho` con la implementacion actual del modelo.
/// `rho` sigue siendo una `RiskFactor::ModelParameter` valida, solo que unicamente via
/// bump-and-reval (la tabla de capacidades de `engine::greeks` en C++ simplemente no declara
/// `aad_supported` para `rho`, ver greeks.cpp).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HullWhite2FGreeks {
    pub d_a: f64,
    pub d_b: f64,
    pub d_sigma: f64,
    pub d_eta: f64,
    pub d_r0: f64,
}

/// Equivalente de `irs_hull_white_npv_all_greeks` para Hull-White 2 factores -- una unica pasada
/// `backward()` para `a`/`b`/`sigma`/`eta`/`r0` (`rho` queda fuera, ver `HullWhite2FGreeks`).
/// Mismo patron modelo "plano" (construccion del swap) / modelo "diff" (NPV diferenciado) que
/// `irs_hull_white_2f_npv_delta_r0` generaliza de un parametro a cinco.
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_2f_npv_all_greeks(
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
) -> HullWhite2FGreeks {
    type AD = Autodiff<CpuBackend>;
    let device = burn::tensor::Device::<AD>::default();

    let plain_model: HullWhite2F<AD> =
        HullWhite2F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device), scalar(eta, &device), rho, scalar(r0, &device));
    let swap = build_irs_swap_2f(&device, &plain_model, notional, fixed_rate, use_par_rate, start, &payment_times, &accruals);

    let a_var: Tensor<AD, 1> = scalar(a, &device).require_grad();
    let b_var: Tensor<AD, 1> = scalar(b, &device).require_grad();
    let sigma_var: Tensor<AD, 1> = scalar(sigma, &device).require_grad();
    let eta_var: Tensor<AD, 1> = scalar(eta, &device).require_grad();
    let r0_var: Tensor<AD, 1> = scalar(r0, &device).require_grad();
    let diff_model: HullWhite2F<AD> =
        HullWhite2F::new(a_var.clone(), b_var.clone(), sigma_var.clone(), eta_var.clone(), rho, r0_var.clone());

    let state0 = (scalar(0.0, &device), scalar(0.0, &device));
    let price = swap.npv(state0, 0.0, &diff_model);
    let grads = price.backward();
    HullWhite2FGreeks {
        d_a: a_var.grad(&grads).unwrap().into_scalar(),
        d_b: b_var.grad(&grads).unwrap().into_scalar(),
        d_sigma: sigma_var.grad(&grads).unwrap().into_scalar(),
        d_eta: eta_var.grad(&grads).unwrap().into_scalar(),
        d_r0: r0_var.grad(&grads).unwrap().into_scalar(),
    }
}

/// Calibra `a`/`b` de `HullWhite1F` a una curva de mercado (`pillars`/`zero_rates`, mismo
/// largo, `pillars` estrictamente creciente) partiendo de `(initial_a, initial_b)`; `sigma`/
/// `r0` no se calibran, ver `crate::calibration` para el porqué. Traduce
/// `crate::curve::Curve` (que ya es `f64` puro) a esta frontera solo para mantener
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
    let curve = crate::curve::Curve::new(pillars, zero_rates);
    crate::calibration::calibrate_hull_white(&curve, initial_a, initial_b, sigma, r0)
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
    let curve = crate::curve::Curve::new(pillars, zero_rates);
    crate::calibration::calibrate_hull_white_2f(&curve, initial_a, initial_b, sigma, eta, rho, r0)
}

// --- Diagnóstico de trayectorias Monte Carlo (PLAN_IMPROVE_NOTEBOOK.md Fase 0) --------------
//
// `Engine.simulate_paths` (Python) es una herramienta de notebook/diagnóstico, NO una medida de
// producción (nunca pasa por `Registry<IMeasure>`, ver el doc-comment de `engine::PathMatrix` en
// `cpp/engine/include/engine/engine.hpp`) -- expone la matriz completa de trayectorias que
// `Gbm::simulate_at_times`/`GbmP::simulate_at_times` ya calculan por dentro para las medidas
// `Payoff*Q`/`Payoff*P`, en vez de solo el agregado final que consumen esas medidas.

/// Tope duro de `n_paths`/`n_steps` para `simulate_paths_gbm_q`/`simulate_paths_gbm_p`
/// (PLAN_IMPROVE_NOTEBOOK.md Fase 0, línea 56-57: "no reventar memoria si alguien lo llama desde
/// Excel/C ABI sin darse cuenta" -- aunque esta fase NO expone estas dos funciones a Excel/C ABI,
/// el límite vive aquí, en el core Rust, para proteger a CUALQUIER llamante presente o futuro,
/// no solo al binding Python de hoy). El producto `SIMULATE_PATHS_MAX_PATHS *
/// (SIMULATE_PATHS_MAX_STEPS + 1) * 8 bytes` = 50_000 * 501 * 8 ≈ 200 MB para la matriz
/// aplanada -- generoso para explorar un
/// fan chart interactivo en un notebook, acotado para no agotar memoria de un proceso normal.
pub const SIMULATE_PATHS_MAX_PATHS: u64 = 50_000;
pub const SIMULATE_PATHS_MAX_STEPS: u64 = 500;

fn check_simulate_paths_limits(n_paths: u64, n_steps: u64, maturity: f64) -> Result<(), String> {
    if n_paths == 0 {
        return Err("simulate_paths: n_paths debe ser > 0".to_string());
    }
    if n_steps == 0 {
        return Err("simulate_paths: n_steps debe ser > 0".to_string());
    }
    if !maturity.is_finite() || maturity <= 0.0 {
        return Err(format!("simulate_paths: maturity ({maturity}) debe ser finito y > 0"));
    }
    if n_paths > SIMULATE_PATHS_MAX_PATHS || n_steps > SIMULATE_PATHS_MAX_STEPS {
        return Err(format!(
            "simulate_paths: n_paths ({n_paths}) x n_steps ({n_steps}) excede el tope duro de \
             diagnostico ({SIMULATE_PATHS_MAX_PATHS} x {SIMULATE_PATHS_MAX_STEPS}, \
             PLAN_IMPROVE_NOTEBOOK.md Fase 0) -- reduce n_paths/n_steps, esta funcion es una \
             herramienta de inspeccion de trayectorias para notebooks, no un pricer de produccion"
        ));
    }
    Ok(())
}

/// Matriz cruda de trayectorias simuladas, `times.len() == n_steps + 1` (incluye `t=0`, `S0`
/// repetido sin simular) x `n_paths` rutas. **Orden de aplanado: ROW-MAJOR POR PATH** --
/// `paths_flat[path * (n_steps + 1) + step]` es el valor de la ruta `path` en `times[step]` --
/// mismo criterio documentado en el struct `ffi::PathMatrixResult` (`engine-ffi/src/lib.rs`) y en
/// `engine::PathMatrix` (C++) y en el docstring de `Engine.simulate_paths` (Python): ninguna capa
/// reordena, todas asumen esta misma convención.
#[derive(Debug, Clone)]
pub struct PathMatrix {
    pub times: Vec<f64>,
    pub paths_flat: Vec<f64>,
    pub n_paths: u64,
    pub n_steps: u64,
}

/// Trayectorias crudas de `Gbm` (medida Q) en una malla uniforme `[0, maturity]` de `n_steps`
/// intervalos (PLAN_IMPROVE_NOTEBOOK.md Fase 0): `times = [0, dt, 2*dt, ..., maturity]` con
/// `dt = maturity / n_steps`. `t=0` se antepone a mano (`S0` conocido, no simulado) porque
/// `Gbm::simulate_at_times` exige `times[0] > 0`; el resto de la malla (`dt..=maturity`) se pasa
/// tal cual a esa función -- la MISMA discretización lognormal exacta que usan
/// `payoff::api::simulate_gbm_columns`/las medidas `Payoff*Q`, no una reimplementación paralela.
#[allow(clippy::too_many_arguments)]
pub fn simulate_paths_gbm_q(
    backend: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    maturity: f64,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> Result<PathMatrix, String> {
    check_simulate_paths_limits(n_paths, n_steps, maturity)?;
    Ok(match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            simulate_paths_gbm_q_on::<CpuBackend>(&device, s0, r, q, sigma, maturity, n_steps, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                simulate_paths_gbm_q_on::<crate::backend::GpuBackend>(
                    &device, s0, r, q, sigma, maturity, n_steps, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                simulate_paths_gbm_q_on::<CpuBackend>(&device, s0, r, q, sigma, maturity, n_steps, n_paths, seed)
            }
        }
    })
}

#[allow(clippy::too_many_arguments)]
fn simulate_paths_gbm_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    maturity: f64,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> PathMatrix {
    B::seed(device, seed);
    let dt = maturity / n_steps as f64;
    let times: Vec<f64> = (1..=n_steps).map(|i| i as f64 * dt).collect();
    let model = crate::models::gbm::Gbm::<B>::new(scalar(s0, device), scalar(r, device), scalar(q, device), scalar(sigma, device));
    let simulated = model.simulate_at_times(&times, n_paths as usize, device);
    flatten_paths(s0, &times, simulated, n_paths, n_steps)
}

/// Trayectorias crudas de `GbmP` (medida física P, drift `mu`) -- equivalente de
/// `simulate_paths_gbm_q` bajo P, ver su doc-comment para la construcción de la malla y el
/// porqué de anteponer `t=0` a mano.
#[allow(clippy::too_many_arguments)]
pub fn simulate_paths_gbm_p(
    backend: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    maturity: f64,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> Result<PathMatrix, String> {
    check_simulate_paths_limits(n_paths, n_steps, maturity)?;
    Ok(match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            simulate_paths_gbm_p_on::<CpuBackend>(&device, s0, mu, sigma, maturity, n_steps, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                simulate_paths_gbm_p_on::<crate::backend::GpuBackend>(&device, s0, mu, sigma, maturity, n_steps, n_paths, seed)
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                simulate_paths_gbm_p_on::<CpuBackend>(&device, s0, mu, sigma, maturity, n_steps, n_paths, seed)
            }
        }
    })
}

#[allow(clippy::too_many_arguments)]
fn simulate_paths_gbm_p_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    s0: f64,
    mu: f64,
    sigma: f64,
    maturity: f64,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> PathMatrix {
    B::seed(device, seed);
    let dt = maturity / n_steps as f64;
    let times: Vec<f64> = (1..=n_steps).map(|i| i as f64 * dt).collect();
    let model = crate::models::gbm_p::GbmP::<B>::new(scalar(s0, device), scalar(mu, device), scalar(sigma, device));
    let simulated = model.simulate_at_times(&times, n_paths as usize, device);
    flatten_paths(s0, &times, simulated, n_paths, n_steps)
}

/// Comparte la materialización final (tensor -> `Vec<f64>` -> matriz aplanada row-major-por-path)
/// entre `simulate_paths_gbm_q_on`/`simulate_paths_gbm_p_on` -- `simulated[step]` es un tensor
/// `[n_paths]` (uno por cada instante de `times`, SIN `t=0`, ver el doc-comment de
/// `Gbm::simulate_at_times`); esta función antepone `t=0`/`s0` y transpone a la convención
/// documentada en `PathMatrix`.
fn flatten_paths<B: Backend<FloatElem = f64>>(
    s0: f64,
    times: &[f64],
    simulated: Vec<Tensor<B, 1>>,
    n_paths: u64,
    n_steps: u64,
) -> PathMatrix {
    let columns: Vec<Vec<f64>> = simulated.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect();
    let cols = n_steps as usize + 1;
    let n_paths_usize = n_paths as usize;

    let mut full_times = Vec::with_capacity(cols);
    full_times.push(0.0);
    full_times.extend_from_slice(times);

    let mut paths_flat = vec![0.0_f64; n_paths_usize * cols];
    for path in 0..n_paths_usize {
        paths_flat[path * cols] = s0;
        for (step, column) in columns.iter().enumerate() {
            paths_flat[path * cols + step + 1] = column[path];
        }
    }

    PathMatrix { times: full_times, paths_flat, n_paths, n_steps }
}

// --- GbmBasket (PLAN_IMPROVE_NOTEBOOK2.md Fase 4) --------------------------------------------
//
// Generaliza `simulate_paths_gbm_q` (un unico observable) a `n_assets` observables
// correlacionados via `models::gbm_basket::GbmBasket::simulate_at_times` (misma funcion de
// simulacion que ya usa `payoff::basket_api::price_payoff_basket_gbm_q_on` internamente para
// "PayoffPriceQ" -- aqui se expone la matriz de trayectorias CRUDA, no solo el agregado
// descontado, mismo criterio que `simulate_paths_gbm_q` respecto de `Gbm`).

/// Matriz cruda de trayectorias de un `GbmBasket` de `n_assets` activos: `times.len() ==
/// n_steps + 1` (incluye `t=0`, `S0` repetido sin simular, mismo criterio que `PathMatrix`).
/// **Orden de aplanado: ROW-MAJOR POR (path, step, asset)** -- el índice es
/// `paths_flat[path * (n_steps + 1) * n_assets + step * n_assets + asset]`; contiene el valor
/// del activo `asset` de la ruta `path` en
/// `times[step]` -- convencion elegida para que la capa Python solo necesite un `reshape((n_paths,
/// n_steps + 1, n_assets))` en vez de un `reshape` + `transpose`, MISMA convencion documentada en
/// `ffi::BasketPathMatrixResult` (`engine-ffi/src/lib.rs`), `engine::BasketPathMatrix` (C++) y el
/// docstring de `Engine.simulate_paths` (Python) -- ninguna capa reordena.
#[derive(Debug, Clone)]
pub struct BasketPathMatrix {
    pub times: Vec<f64>,
    pub paths_flat: Vec<f64>,
    pub n_paths: u64,
    pub n_steps: u64,
    pub n_assets: u64,
}

/// Trayectorias crudas de `GbmBasket` (medida Q) en una malla uniforme `[0, maturity]` de
/// `n_steps` intervalos -- mismos limites/preflight que `simulate_paths_gbm_q`
/// (`check_simulate_paths_limits`, PLAN_IMPROVE_NOTEBOOK.md Fase 0), mas la validacion de forma
/// de `GbmBasket::new` (longitudes de `s0`/`r`/`q`/`sigma` y que `correlation` sea PSD).
/// `s0`/`r`/`q`/`sigma` uno por activo (define `n_assets = s0.len()`); `correlation_flat` es la
/// matriz de correlacion aplanada FILA A FILA, `n_assets x n_assets` (mismo convenio que
/// `payoff::basket_api::price_payoff_basket_gbm_q`) -- `Err` explicito ANTES de simular si
/// `correlation_flat.len() != n_assets * n_assets` (evita un panic de slicing fuera de rango).
#[allow(clippy::too_many_arguments)]
pub fn simulate_paths_gbm_basket_q(
    backend: &str,
    s0: &[f64],
    r: &[f64],
    q: &[f64],
    sigma: &[f64],
    correlation_flat: &[f64],
    maturity: f64,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> Result<BasketPathMatrix, String> {
    check_simulate_paths_limits(n_paths, n_steps, maturity)?;
    let n_assets = s0.len();
    if n_assets == 0 {
        return Err("simulate_paths_gbm_basket_q: 's0' no puede estar vacio (se requiere al menos un activo)".to_string());
    }
    if correlation_flat.len() != n_assets * n_assets {
        return Err(format!(
            "simulate_paths_gbm_basket_q: 'correlation' aplanada debe tener {n}x{n}={} elementos (uno por activo \
             declarado en 's0'), se recibieron {}",
            n_assets * n_assets,
            correlation_flat.len(),
            n = n_assets
        ));
    }
    Ok(match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            simulate_paths_gbm_basket_q_on::<CpuBackend>(
                &device, s0, r, q, sigma, correlation_flat, maturity, n_steps, n_paths, seed,
            )?
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                simulate_paths_gbm_basket_q_on::<crate::backend::GpuBackend>(
                    &device, s0, r, q, sigma, correlation_flat, maturity, n_steps, n_paths, seed,
                )?
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                simulate_paths_gbm_basket_q_on::<CpuBackend>(
                    &device, s0, r, q, sigma, correlation_flat, maturity, n_steps, n_paths, seed,
                )?
            }
        }
    })
}

#[allow(clippy::too_many_arguments)]
fn simulate_paths_gbm_basket_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    s0: &[f64],
    r: &[f64],
    q: &[f64],
    sigma: &[f64],
    correlation_flat: &[f64],
    maturity: f64,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> Result<BasketPathMatrix, String> {
    B::seed(device, seed);
    let dt = maturity / n_steps as f64;
    let times: Vec<f64> = (1..=n_steps).map(|i| i as f64 * dt).collect();
    let n_assets = s0.len();

    let s0_t: Vec<Tensor<B, 1>> = s0.iter().map(|&v| scalar(v, device)).collect();
    let r_t: Vec<Tensor<B, 1>> = r.iter().map(|&v| scalar(v, device)).collect();
    let q_t: Vec<Tensor<B, 1>> = q.iter().map(|&v| scalar(v, device)).collect();
    let sigma_t: Vec<Tensor<B, 1>> = sigma.iter().map(|&v| scalar(v, device)).collect();
    let correlation: Vec<Vec<f64>> =
        (0..n_assets).map(|i| correlation_flat[i * n_assets..(i + 1) * n_assets].to_vec()).collect();
    let model = crate::models::gbm_basket::GbmBasket::<B>::new(s0_t, r_t, q_t, sigma_t, correlation)?;

    // simulated[step][asset] -> Tensor<B,1> forma [n_paths] (times SIN t=0, ver el doc-comment de
    // GbmBasket::simulate_at_times).
    let simulated = model.simulate_at_times(&times, n_paths as usize, device);
    Ok(flatten_basket_paths::<B>(s0, &times, simulated, n_paths, n_steps, n_assets as u64))
}

/// Equivalente de `flatten_paths` para `GbmBasket`: antepone `t=0`/`s0` (uno por activo) y
/// aplana a la convencion `(path, step, asset)` documentada en `BasketPathMatrix`.
fn flatten_basket_paths<B: Backend<FloatElem = f64>>(
    s0: &[f64],
    times: &[f64],
    simulated: Vec<Vec<Tensor<B, 1>>>,
    n_paths: u64,
    n_steps: u64,
    n_assets: u64,
) -> BasketPathMatrix {
    // columns[step][asset] -> Vec<f64> (longitud n_paths)
    let columns: Vec<Vec<Vec<f64>>> = simulated
        .into_iter()
        .map(|per_asset| per_asset.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect())
        .collect();
    let cols = n_steps as usize + 1;
    let n_assets_usize = n_assets as usize;
    let n_paths_usize = n_paths as usize;

    let mut full_times = Vec::with_capacity(cols);
    full_times.push(0.0);
    full_times.extend_from_slice(times);

    let mut paths_flat = vec![0.0_f64; n_paths_usize * cols * n_assets_usize];
    for path in 0..n_paths_usize {
        let base = path * cols * n_assets_usize;
        for (asset, &s0_asset) in s0.iter().enumerate() {
            paths_flat[base + asset] = s0_asset;
        }
        for (step, per_asset_at_step) in columns.iter().enumerate() {
            let step_base = base + (step + 1) * n_assets_usize;
            for (asset, column) in per_asset_at_step.iter().enumerate() {
                paths_flat[step_base + asset] = column[path];
            }
        }
    }

    BasketPathMatrix { times: full_times, paths_flat, n_paths, n_steps, n_assets }
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
    fn irs_hull_white_npv_all_greeks_d_r0_matches_the_single_greek_function() {
        // La derivada d_r0 de la pasada unica debe coincidir EXACTAMENTE (mismo grafo, mismo
        // backward, ninguna aproximacion de por medio) con irs_hull_white_npv_delta_r0.
        let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
        let (notional, fixed_rate, start) = (1_000_000.0, 0.0, 0.0);
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];

        let all_greeks = irs_hull_white_npv_all_greeks(
            a, b, sigma, r0, notional, fixed_rate, true, start, payment_times.clone(), accruals.clone(),
        );
        let single_delta = irs_hull_white_npv_delta_r0(
            a, b, sigma, r0, notional, fixed_rate, true, start, payment_times, accruals,
        );
        assert!(
            (all_greeks.d_r0 - single_delta).abs() < 1e-9,
            "d_r0={} single={single_delta}", all_greeks.d_r0
        );
    }

    #[test]
    fn irs_hull_white_npv_all_greeks_matches_bump_and_reval_for_a_b_sigma() {
        // Test diferencial obligatorio de PLAN_GREEKS.md §5.3: AAD reverse-mode vs bump-and-reval
        // (diferencia central) para CADA parametro. Tolerancia RELATIVA (1e-6): el notional
        // (1e6) escala tanto el NPV como sus derivadas, asi que una tolerancia absoluta fija no
        // es comparable entre fixtures de distinto tamano -- el error de truncamiento de la
        // diferencia central es del orden de la propia magnitud, no una constante.
        let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
        let (notional, fixed_rate, start) = (1_000_000.0, 0.02, 0.0);
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let h = 1e-4;

        let greeks = irs_hull_white_npv_all_greeks(
            a, b, sigma, r0, notional, fixed_rate, false, start, payment_times.clone(), accruals.clone(),
        );

        let npv = |a: f64, b: f64, sigma: f64, r0: f64| {
            irs_hull_white_npv(a, b, sigma, r0, notional, fixed_rate, false, start, payment_times.clone(), accruals.clone())
        };
        let bump_and_reval_a = (npv(a + h, b, sigma, r0) - npv(a - h, b, sigma, r0)) / (2.0 * h);
        let bump_and_reval_b = (npv(a, b + h, sigma, r0) - npv(a, b - h, sigma, r0)) / (2.0 * h);
        let bump_and_reval_sigma = (npv(a, b, sigma + h, r0) - npv(a, b, sigma - h, r0)) / (2.0 * h);

        let tol = |reference: f64| 1e-6 * reference.abs().max(1.0);
        assert!(
            (greeks.d_a - bump_and_reval_a).abs() < tol(bump_and_reval_a),
            "d_a={} bump_and_reval={bump_and_reval_a}", greeks.d_a
        );
        assert!(
            (greeks.d_b - bump_and_reval_b).abs() < tol(bump_and_reval_b),
            "d_b={} bump_and_reval={bump_and_reval_b}", greeks.d_b
        );
        assert!(
            (greeks.d_sigma - bump_and_reval_sigma).abs() < tol(bump_and_reval_sigma),
            "d_sigma={} bump_and_reval={bump_and_reval_sigma}", greeks.d_sigma
        );
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
    fn irs_hull_white_2f_npv_all_greeks_d_r0_matches_the_single_greek_function() {
        let (a, b, sigma, eta, rho, r0) = (0.1, 0.2, 0.01, 0.012, -0.7, 0.03);
        let (notional, fixed_rate, start) = (1_000_000.0, 0.0, 0.0);
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];

        let all_greeks = irs_hull_white_2f_npv_all_greeks(
            a, b, sigma, eta, rho, r0, notional, fixed_rate, true, start, payment_times.clone(), accruals.clone(),
        );
        let single_delta = irs_hull_white_2f_npv_delta_r0(
            a, b, sigma, eta, rho, r0, notional, fixed_rate, true, start, payment_times, accruals,
        );
        assert!(
            (all_greeks.d_r0 - single_delta).abs() < 1e-9,
            "d_r0={} single={single_delta}", all_greeks.d_r0
        );
    }

    #[test]
    fn irs_hull_white_2f_npv_all_greeks_matches_bump_and_reval_for_a_b_sigma_eta() {
        let (a, b, sigma, eta, rho, r0) = (0.1, 0.2, 0.01, 0.012, -0.7, 0.03);
        let (notional, fixed_rate, start) = (1_000_000.0, 0.02, 0.0);
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0; 5];
        let h = 1e-4;

        let greeks = irs_hull_white_2f_npv_all_greeks(
            a, b, sigma, eta, rho, r0, notional, fixed_rate, false, start, payment_times.clone(), accruals.clone(),
        );

        let npv = |a: f64, b: f64, sigma: f64, eta: f64| {
            irs_hull_white_2f_npv(
                a, b, sigma, eta, rho, r0, notional, fixed_rate, false, start, payment_times.clone(), accruals.clone(),
            )
        };
        let bump_and_reval_a = (npv(a + h, b, sigma, eta) - npv(a - h, b, sigma, eta)) / (2.0 * h);
        let bump_and_reval_b = (npv(a, b + h, sigma, eta) - npv(a, b - h, sigma, eta)) / (2.0 * h);
        let bump_and_reval_sigma = (npv(a, b, sigma + h, eta) - npv(a, b, sigma - h, eta)) / (2.0 * h);
        let bump_and_reval_eta = (npv(a, b, sigma, eta + h) - npv(a, b, sigma, eta - h)) / (2.0 * h);

        // Misma tolerancia relativa que irs_hull_white_npv_all_greeks_matches_bump_and_reval_for_a_b_sigma.
        let tol = |reference: f64| 1e-6 * reference.abs().max(1.0);
        assert!((greeks.d_a - bump_and_reval_a).abs() < tol(bump_and_reval_a), "d_a={} bump_and_reval={bump_and_reval_a}", greeks.d_a);
        assert!((greeks.d_b - bump_and_reval_b).abs() < tol(bump_and_reval_b), "d_b={} bump_and_reval={bump_and_reval_b}", greeks.d_b);
        assert!(
            (greeks.d_sigma - bump_and_reval_sigma).abs() < tol(bump_and_reval_sigma),
            "d_sigma={} bump_and_reval={bump_and_reval_sigma}", greeks.d_sigma
        );
        assert!(
            (greeks.d_eta - bump_and_reval_eta).abs() < tol(bump_and_reval_eta),
            "d_eta={} bump_and_reval={bump_and_reval_eta}", greeks.d_eta
        );
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
        let curve = crate::curve::Curve::synthetic_from_hull_white(true_a, true_b, sigma, r0, pillars.clone());

        let result = calibrate_hull_white(pillars, curve.zero_rates().to_vec(), 0.3, 0.01, sigma, r0);

        assert!(result.converged, "no convergió: rmse={}", result.rmse);
        assert!((result.a - true_a).abs() < 1e-4);
        assert!((result.b - true_b).abs() < 1e-4);
    }

    #[test]
    fn calibrate_hull_white_2f_recovers_known_parameters() {
        let (true_a, true_b, sigma, eta, rho, r0) = (0.15, 0.25, 0.008, 0.01, -0.6, 0.02);
        let pillars = vec![1.0, 2.0, 5.0, 10.0, 20.0];
        let curve =
            crate::curve::Curve::synthetic_from_hull_white_2f(true_a, true_b, sigma, eta, rho, r0, pillars.clone());

        let result = calibrate_hull_white_2f(pillars, curve.zero_rates().to_vec(), 0.4, 0.05, sigma, eta, rho, r0);

        assert!(result.converged, "no convergió: rmse={}", result.rmse);
        assert!((result.a - true_a).abs() < 1e-4);
        assert!((result.b - true_b).abs() < 1e-4);
    }

    // --- simulate_paths_gbm_q/_p (PLAN_IMPROVE_NOTEBOOK.md Fase 0) --------------------------

    #[test]
    fn simulate_paths_gbm_q_shape_and_t0_column_equal_s0() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, r, q, sigma, maturity) = (100.0, 0.05, 0.0, 0.2, 1.0);
        let (n_steps, n_paths) = (10_u64, 1_000_u64);
        let pm = simulate_paths_gbm_q("cpu", s0, r, q, sigma, maturity, n_steps, n_paths, 42).unwrap();

        assert_eq!(pm.times.len(), (n_steps + 1) as usize);
        assert_eq!(pm.times[0], 0.0);
        assert!((pm.times.last().unwrap() - maturity).abs() < 1e-12);
        assert_eq!(pm.paths_flat.len(), (n_paths * (n_steps + 1)) as usize);
        assert_eq!(pm.n_paths, n_paths);
        assert_eq!(pm.n_steps, n_steps);

        // Convencion row-major por path: paths_flat[path*(n_steps+1)] es la columna t=0 de esa
        // ruta -- S0 conocido, identico para todas las rutas (no simulado).
        let cols = (n_steps + 1) as usize;
        for path in 0..n_paths as usize {
            assert_eq!(pm.paths_flat[path * cols], s0);
        }
    }

    #[test]
    fn simulate_paths_gbm_q_terminal_moments_match_lognormal_gbm_formula() {
        // PLAN_IMPROVE_NOTEBOOK.md Fase 0, criterio de aceptacion: media/varianza de la matriz
        // simulada en t=T deben coincidir con la formula analitica del GBM lognormal, mismo
        // criterio de tolerancia que models::gbm::tests::simulate_at_times_starts_from_s0_in_expectation.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, r, q, sigma, maturity) = (100.0, 0.05, 0.01, 0.25, 2.0);
        let (n_steps, n_paths) = (20_u64, 20_000_u64);
        let pm = simulate_paths_gbm_q("cpu", s0, r, q, sigma, maturity, n_steps, n_paths, 7).unwrap();

        let cols = (n_steps + 1) as usize;
        let terminal: Vec<f64> = (0..n_paths as usize).map(|path| pm.paths_flat[path * cols + n_steps as usize]).collect();
        let n = n_paths as f64;
        let mean: f64 = terminal.iter().sum::<f64>() / n;
        let variance: f64 = terminal.iter().map(|s| (s - mean).powi(2)).sum::<f64>() / (n - 1.0);
        let std_error = (variance / n).sqrt();

        // E_Q[S_T] = S0 * exp((r-q)*T) -- invariante de martingala bajo Q, formula cerrada del
        // GBM lognormal (mismo oraculo que crate::models::gbm).
        let expected_mean = s0 * ((r - q) * maturity).exp();
        assert!(
            (mean - expected_mean).abs() < 6.0 * std_error,
            "mean={mean} expected={expected_mean} se={std_error}"
        );

        // Var_Q[S_T] = S0^2 * exp(2*(r-q)*T) * (exp(sigma^2*T) - 1) -- varianza cerrada de una
        // lognormal con esos parametros de GBM.
        let expected_variance = s0 * s0 * (2.0 * (r - q) * maturity).exp() * ((sigma * sigma * maturity).exp() - 1.0);
        let relative_error = (variance - expected_variance).abs() / expected_variance;
        assert!(relative_error < 0.1, "variance={variance} expected={expected_variance} rel_err={relative_error}");
    }

    #[test]
    fn simulate_paths_gbm_p_terminal_mean_matches_e_p_s_t_equals_s0_exp_mu_t() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, mu, sigma, maturity) = (100.0, 0.08, 0.2, 1.5);
        let (n_steps, n_paths) = (15_u64, 20_000_u64);
        let pm = simulate_paths_gbm_p("cpu", s0, mu, sigma, maturity, n_steps, n_paths, 99).unwrap();

        let cols = (n_steps + 1) as usize;
        let terminal: Vec<f64> = (0..n_paths as usize).map(|path| pm.paths_flat[path * cols + n_steps as usize]).collect();
        let n = n_paths as f64;
        let mean: f64 = terminal.iter().sum::<f64>() / n;
        let variance: f64 = terminal.iter().map(|s| (s - mean).powi(2)).sum::<f64>() / (n - 1.0);
        let std_error = (variance / n).sqrt();

        let expected_mean = s0 * (mu * maturity).exp();
        assert!(
            (mean - expected_mean).abs() < 6.0 * std_error,
            "mean={mean} expected={expected_mean} se={std_error}"
        );
    }

    #[test]
    fn simulate_paths_rejects_n_paths_or_n_steps_over_the_hard_cap() {
        let over_paths = simulate_paths_gbm_q(
            "cpu", 100.0, 0.05, 0.0, 0.2, 1.0, 10, SIMULATE_PATHS_MAX_PATHS + 1, 1,
        );
        assert!(over_paths.is_err(), "n_paths por encima del tope debe rechazarse");

        let over_steps = simulate_paths_gbm_q(
            "cpu", 100.0, 0.05, 0.0, 0.2, 1.0, SIMULATE_PATHS_MAX_STEPS + 1, 10, 1,
        );
        assert!(over_steps.is_err(), "n_steps por encima del tope debe rechazarse");
    }

    #[test]
    fn simulate_paths_rejects_zero_n_paths_zero_n_steps_or_non_positive_maturity() {
        assert!(simulate_paths_gbm_q("cpu", 100.0, 0.05, 0.0, 0.2, 1.0, 10, 0, 1).is_err());
        assert!(simulate_paths_gbm_q("cpu", 100.0, 0.05, 0.0, 0.2, 1.0, 0, 10, 1).is_err());
        assert!(simulate_paths_gbm_q("cpu", 100.0, 0.05, 0.0, 0.2, 0.0, 10, 10, 1).is_err());
        assert!(simulate_paths_gbm_q("cpu", 100.0, 0.05, 0.0, 0.2, -1.0, 10, 10, 1).is_err());
    }

    // --- simulate_paths_gbm_basket_q (PLAN_IMPROVE_NOTEBOOK2.md Fase 4) ---------------------

    #[test]
    fn simulate_paths_gbm_basket_q_shape_and_t0_columns_equal_s0_per_asset() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, r, q, sigma, maturity) = ([100.0, 50.0], [0.03, 0.03], [0.0, 0.0], [0.2, 0.35], 1.0);
        let (n_steps, n_paths) = (10_u64, 1_000_u64);
        let n_assets = 2_u64;
        let pm = simulate_paths_gbm_basket_q(
            "cpu", &s0, &r, &q, &sigma, &[1.0, 0.4, 0.4, 1.0], maturity, n_steps, n_paths, 42,
        )
        .unwrap();

        assert_eq!(pm.times.len(), (n_steps + 1) as usize);
        assert_eq!(pm.times[0], 0.0);
        assert!((pm.times.last().unwrap() - maturity).abs() < 1e-12);
        assert_eq!(pm.n_paths, n_paths);
        assert_eq!(pm.n_steps, n_steps);
        assert_eq!(pm.n_assets, n_assets);
        assert_eq!(pm.paths_flat.len(), (n_paths * (n_steps + 1) * n_assets) as usize);

        // Convencion (path, step, asset): paths_flat[path*(n_steps+1)*n_assets + asset] es la
        // columna t=0 del activo `asset` de esa ruta -- S0 de ESE activo, identico en todas las
        // rutas (no simulado).
        let cols = (n_steps + 1) as usize;
        let n_assets_usize = n_assets as usize;
        for path in 0..n_paths as usize {
            let base = path * cols * n_assets_usize;
            assert_eq!(pm.paths_flat[base], s0[0]);
            assert_eq!(pm.paths_flat[base + 1], s0[1]);
        }
    }

    #[test]
    fn simulate_paths_gbm_basket_q_terminal_marginal_moments_match_univariate_gbm() {
        // PLAN_IMPROVE_NOTEBOOK2.md Fase 4, criterio de aceptacion: paridad de momentos MARGINALES
        // por activo, mismo criterio que ya exige simulate_paths_gbm_q para el caso N=1 (y que
        // gbm_basket::tests::marginal_moments_match_univariate_gbm_regardless_of_correlation ya
        // exige a nivel de GbmBasket::simulate_at_times).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, r, q, sigma, maturity) = (100.0, 0.04, 0.01, 0.3, 1.5);
        let (n_steps, n_paths) = (12_u64, 30_000_u64);
        let pm = simulate_paths_gbm_basket_q(
            "cpu",
            &[s0, s0],
            &[r, r],
            &[q, q],
            &[sigma, sigma],
            &[1.0, 0.6, 0.6, 1.0],
            maturity,
            n_steps,
            n_paths,
            13,
        )
        .unwrap();

        let cols = (n_steps + 1) as usize;
        let n_assets = 2usize;
        let asset0: Vec<f64> = (0..n_paths as usize)
            .map(|path| pm.paths_flat[path * cols * n_assets + (n_steps as usize) * n_assets])
            .collect();
        let n = n_paths as f64;
        let mean: f64 = asset0.iter().sum::<f64>() / n;
        let variance: f64 = asset0.iter().map(|s| (s - mean).powi(2)).sum::<f64>() / (n - 1.0);
        let std_error = (variance / n).sqrt();

        let expected_mean = s0 * ((r - q) * maturity).exp();
        assert!(
            (mean - expected_mean).abs() < 6.0 * std_error,
            "mean={mean} expected={expected_mean} se={std_error}"
        );
    }

    #[test]
    fn simulate_paths_gbm_basket_q_rejects_mismatched_correlation_shape_before_simulating() {
        let err = simulate_paths_gbm_basket_q(
            "cpu", &[100.0, 100.0], &[0.03, 0.03], &[0.0, 0.0], &[0.2, 0.2], &[1.0, 0.0, 0.0], 1.0, 5, 10, 1,
        )
        .expect_err("correlation 2x1 en vez de 2x2 deberia rechazarse antes de simular");
        assert!(err.contains("correlation"), "err={err}");
    }

    #[test]
    fn simulate_paths_gbm_basket_q_rejects_n_paths_or_n_steps_over_the_hard_cap() {
        let over_paths = simulate_paths_gbm_basket_q(
            "cpu", &[100.0], &[0.05], &[0.0], &[0.2], &[1.0], 1.0, 10, SIMULATE_PATHS_MAX_PATHS + 1, 1,
        );
        assert!(over_paths.is_err(), "n_paths por encima del tope debe rechazarse");
    }
}
