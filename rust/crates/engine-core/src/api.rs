//! Funciones en `f64` puro que expone el registry de la capa C++ (Fase 2, PLAN.md §5.4)
//! sobre el core Rust, sin tipos de Burn en la firma — misma frontera aislada que documenta
//! `crate::smoke`, pero esta ya es la API real: la superficie que `engine-ffi` bridgea al
//! `IMeasure` de la capa C++ (`ExposureProfileMeasure`/`UnilateralCvaMeasure`), no un smoke
//! test del pipeline de build.

use crate::backend::CpuBackend;
use crate::exposure::{expected_exposure_profile, unilateral_cva, ExposureProfile};
use crate::models::hull_white::HullWhite1F;
use crate::products::irs::IrSwap;
use burn::tensor::{Tensor, TensorData};

type Device = burn::tensor::Device<CpuBackend>;

fn scalar(value: f64, device: &Device) -> Tensor<CpuBackend, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Perfil de exposición (EE/PFE) de un IRS arbitrario bajo Hull-White 1F (PLAN.md §5.2),
/// generalización de `crate::smoke::irs_unilateral_cva_5y` (que fija el IRS a 5y anual):
/// aquí `payment_times`/`accruals` son arbitrarios y el resultado expone el perfil completo,
/// no solo el CVA agregado, para que la capa C++ (`measure.hpp`) pueda componer medidas.
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
    let device = Device::default();
    let model: HullWhite1F<CpuBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));

    let rate = if use_par_rate {
        IrSwap::par_rate(scalar(r0, &device), start, &payment_times, &accruals, &model)
    } else {
        scalar(fixed_rate, &device)
    };

    let swap: IrSwap<CpuBackend> = IrSwap {
        notional: scalar(notional, &device),
        fixed_rate: rate,
        start,
        payment_times,
        accruals,
    };

    expected_exposure_profile(&model, &swap, r0, monitoring_times, n_paths, seed, &device)
}

/// CVA unilateral a partir de un perfil de exposición ya calculado (`times`/`ee`, mismo
/// largo) — separa el cálculo del perfil del cálculo del CVA para que la capa C++ pueda
/// componerlas sin recalcular la simulación Monte Carlo.
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
    let device = Device::default();
    let model: HullWhite1F<CpuBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));

    let profile = ExposureProfile {
        times,
        ee,
        pfe_95: Vec::new(),
    };
    unilateral_cva(&profile, &model, r0, hazard_rate, recovery_rate, &device)
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
}
