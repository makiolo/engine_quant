//! Funciones en `f64` puro que expone el registry de la capa C++ (Fase 2, PLAN.md §5.4)
//! sobre el core Rust, sin tipos de Burn en la firma — misma frontera aislada que documenta
//! `crate::smoke`, pero esta ya es la API real: la superficie que `engine-ffi` bridgea al
//! `IMeasure` de la capa C++ (`ExposureProfileMeasure`/`UnilateralCvaMeasure`), no un smoke
//! test del pipeline de build.
//!
//! **Selección de backend (PLAN.md §7.12)**: `irs_hull_white_exposure_profile`/
//! `unilateral_cva_from_exposure` no reciben el backend como parámetro — leen el que esté
//! activo en `crate::backend::current()` (estado global de proceso, ver `set_compute_backend`
//! más abajo). Es deliberado: mantiene sin cambios la firma que ya consume `engine-ffi`/C++/
//! Python/Excel y encaja con cómo cada cliente quiere seleccionarlo — un `with` en Python
//! (contexto ambiente, no un argumento que haya que colar en cada llamada) o una UDF global en
//! Excel (`ENGINE.SET_BACKEND`), no un parámetro más de `ENGINE.EVALUATE`.

use crate::backend::{self, ComputeBackend, CpuBackend};
use crate::exposure::{expected_exposure_profile, unilateral_cva, ExposureProfile};
use crate::models::hull_white::HullWhite1F;
use crate::products::irs::IrSwap;
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

fn scalar<B: Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

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
    n_paths: usize,
    seed: u64,
) -> ExposureProfile {
    let model: HullWhite1F<B> = HullWhite1F::new(scalar(a, device), scalar(b, device), scalar(sigma, device));

    let rate = if use_par_rate {
        IrSwap::par_rate(scalar(r0, device), start, payment_times, accruals, &model)
    } else {
        scalar(fixed_rate, device)
    };

    let swap: IrSwap<B> = IrSwap {
        notional: scalar(notional, device),
        fixed_rate: rate,
        start,
        payment_times: payment_times.to_vec(),
        accruals: accruals.to_vec(),
    };

    expected_exposure_profile(&model, &swap, r0, monitoring_times, n_paths, seed, device)
}

/// Perfil de exposición (EE/PFE) de un IRS arbitrario bajo Hull-White 1F (PLAN.md §5.2),
/// generalización de `crate::smoke::irs_unilateral_cva_5y` (que fija el IRS a 5y anual):
/// aquí `payment_times`/`accruals` son arbitrarios y el resultado expone el perfil completo,
/// no solo el CVA agregado, para que la capa C++ (`measure.hpp`) pueda componer medidas.
/// Corre en `CpuBackend` o `GpuBackend` según `crate::backend::current()` (ver docs del
/// módulo).
#[allow(clippy::too_many_arguments)]
pub fn irs_hull_white_exposure_profile(
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
    n_paths: usize,
    seed: u64,
) -> ExposureProfile {
    match backend::current() {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            exposure_profile_on::<CpuBackend>(
                &device, a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
                &payment_times, &accruals, monitoring_times, n_paths, seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                exposure_profile_on::<crate::backend::GpuBackend>(
                    &device, a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
                    &payment_times, &accruals, monitoring_times, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                // Inalcanzable en la práctica: `set_compute_backend("gpu")` ya devuelve
                // `false` sin cambiar `current()` cuando este build no tiene la feature `gpu`
                // (PLAN.md §7.12). CPU como red de seguridad, no como comportamiento normal.
                let device = burn::tensor::Device::<CpuBackend>::default();
                exposure_profile_on::<CpuBackend>(
                    &device, a, b, sigma, r0, notional, fixed_rate, use_par_rate, start,
                    &payment_times, &accruals, monitoring_times, n_paths, seed,
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
/// componerlas sin recalcular la simulación Monte Carlo. Mismo backend que
/// `irs_hull_white_exposure_profile` (ver docs del módulo).
pub fn unilateral_cva_from_exposure(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    match backend::current() {
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

/// Selecciona el backend de cómputo global ("cpu"/"gpu", case-insensitive) que usarán las
/// llamadas siguientes a `irs_hull_white_exposure_profile`/`unilateral_cva_from_exposure`
/// (PLAN.md §7.12). Devuelve `false` sin cambiar nada si `name` no se reconoce o pide un
/// backend no compilado en este build (`is_gpu_backend_available`) — así el cliente puede
/// avisar al usuario en vez de fallar en silencio calculando en CPU sin que nadie lo note.
pub fn set_compute_backend(name: &str) -> bool {
    match backend::parse_backend_name(name) {
        Some(b) => backend::set_current(b),
        None => false,
    }
}

/// Backend de cómputo actualmente seleccionado ("cpu" o "gpu").
pub fn compute_backend_name() -> String {
    backend::current().name().to_string()
}

/// `true` si este build se compiló con soporte GPU (feature `gpu` de `engine-core`),
/// independientemente de cuál sea el backend seleccionado ahora mismo — permite a los
/// clientes distinguir "no lo has activado" de "no está disponible en este build".
pub fn is_gpu_backend_available() -> bool {
    ComputeBackend::Gpu.is_available()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exposure_profile_is_nonnegative_and_pfe_dominates_ee() {
        let profile = irs_hull_white_exposure_profile(
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
            5_000,
            7,
        );
        for i in 0..profile.times.len() {
            assert!(profile.ee[i] >= 0.0);
            assert!(profile.pfe_95[i] >= profile.ee[i]);
        }
    }

    #[test]
    fn cva_from_exposure_is_nonnegative() {
        let profile = irs_hull_white_exposure_profile(
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
            5_000,
            13,
        );
        let cva =
            unilateral_cva_from_exposure(0.1, 0.03, 0.01, 0.02, profile.times, profile.ee, 0.02, 0.4);
        assert!(cva >= 0.0);
    }

    #[test]
    fn cva_from_exposure_is_zero_when_hazard_rate_is_zero() {
        let profile = irs_hull_white_exposure_profile(
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
            2_000,
            17,
        );
        let cva =
            unilateral_cva_from_exposure(0.1, 0.03, 0.01, 0.02, profile.times, profile.ee, 0.0, 0.4);
        assert!(cva.abs() < 1e-9, "CVA con hazard=0 debería ser 0, got {cva}");
    }

    #[test]
    fn set_compute_backend_rejects_unknown_name() {
        assert!(!set_compute_backend("tpu"));
    }

    // "cpu" es el backend por defecto: fijarlo explícitamente es un no-op de estado, seguro
    // de ejecutar en paralelo con el resto de tests de este fichero (todos asumen CPU).
    #[test]
    fn set_compute_backend_cpu_is_a_safe_noop_and_reports_correctly() {
        assert!(set_compute_backend("CPU"));
        assert_eq!(compute_backend_name(), "cpu");
    }

    #[test]
    fn is_gpu_backend_available_matches_feature_flag() {
        assert_eq!(is_gpu_backend_available(), cfg!(feature = "gpu"));
    }
}
