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
use crate::exposure::{expected_exposure_profile, unilateral_cva, ExposureProfile};
use crate::models::hull_white::HullWhite1F;
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exposure_profile_is_nonnegative_and_pfe_dominates_ee() {
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
}
