//! Perfil de exposición (EE/PFE) vía Monte Carlo y CVA unilateral simple sobre ese
//! perfil — PLAN.md §5.2: primera métrica XVA end-to-end del prototipo (IRS +
//! Hull-White). En Fase 2 esto se convierte en una "métrica XVA" registrable desde la
//! capa C++ (PLAN.md §5.4); aquí vive como función libre genérica sobre `B: Backend`
//! (PLAN.md §6 Fase 5: el mismo código sirve para `CpuBackend` y, tras la feature `gpu`,
//! `GpuBackend`, ver `benches/exposure_backend.rs`) porque las sensibilidades de
//! exposición (AAD de EE/CVA) no son parte del alcance de Fase 1 (§5.3 solo pide AAD
//! para la valoración puntual, no para el perfil completo) — el perfil se calcula con
//! Monte Carlo vectorizado (todos los paths a la vez, ver `crate::models::hull_white`) y
//! se reduce a `Vec<f64>` en Rust plano para EE/PFE, sin necesidad de mantener el grafo
//! de cómputo de Burn más allá de ese punto.

use crate::models::hull_white::HullWhite1F;
use crate::products::irs::IrSwap;
use burn::tensor::backend::Backend;
use burn::tensor::{Distribution, Tensor, TensorData};

fn scalar<B: Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

fn to_vec<B: Backend>(t: Tensor<B, 1>) -> Vec<f64> {
    t.into_data().to_vec::<f64>().unwrap()
}

/// Perfil de exposición esperada (EE) y exposición potencial futura al 95% (PFE) en un
/// conjunto de fechas de monitorización.
#[derive(Debug, Clone)]
pub struct ExposureProfile {
    pub times: Vec<f64>,
    pub ee: Vec<f64>,
    pub pfe_95: Vec<f64>,
}

/// Calcula el perfil EE/PFE de `swap` bajo `model`, simulando el tipo corto por Monte
/// Carlo (PLAN.md §5.2, vectorizado sobre los `n_paths` a la vez vía tensores Burn) sobre
/// una malla fina hasta la última fecha de monitorización, y revalorando en cada una el
/// swap *restante* (`IrSwap::remaining_from`) analíticamente vía `IrSwap::npv`.
///
/// Cada fecha de `monitoring_times` debe ser una fecha de reseteo válida del swap
/// (`swap.is_reset_date`, ver limitación documentada en `crate::products::irs`): la
/// pata flotante debe fijar su próximo cupón exactamente ahí para que la fórmula de
/// réplica en bonos siga siendo válida.
pub fn expected_exposure_profile<B: Backend<FloatElem = f64>>(
    model: &HullWhite1F<B>,
    swap: &IrSwap<B>,
    r0: f64,
    monitoring_times: &[f64],
    n_paths: usize,
    seed: u64,
    device: &burn::tensor::Device<B>,
) -> ExposureProfile {
    assert!(!monitoring_times.is_empty(), "monitoring_times vacío");
    for &t in monitoring_times {
        assert!(
            swap.is_reset_date(t),
            "t={t} no es una fecha de reseteo válida del swap"
        );
    }
    let t_max = monitoring_times.iter().cloned().fold(f64::MIN, f64::max);

    let remaining_swaps: Vec<IrSwap<B>> =
        monitoring_times.iter().map(|&t| swap.remaining_from(t)).collect();

    // Con t_max = 0 (única fecha de monitorización = hoy) no hace falta simular nada:
    // todas las trayectorias arrancan en r0.
    if t_max <= 0.0 {
        let npv = remaining_swaps[0]
            .npv(scalar(r0, device), monitoring_times[0], model)
            .into_scalar()
            .max(0.0);
        return ExposureProfile {
            times: monitoring_times.to_vec(),
            ee: vec![npv],
            pfe_95: vec![npv],
        };
    }

    // Malla fina común (~1 paso/semana) para simular el tipo corto; las fechas de
    // monitorización se muestrean al índice de grid más cercano.
    let n_steps = ((t_max / (1.0 / 52.0)).ceil() as usize).max(monitoring_times.len());
    let dt = t_max / n_steps as f64;

    B::seed(device, seed);
    let shocks: Vec<Tensor<B, 1>> = (0..n_steps)
        .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), device))
        .collect();
    let path = model.simulate_path(scalar(r0, device), dt, &shocks);

    let mut ee = Vec::with_capacity(monitoring_times.len());
    let mut pfe_95 = Vec::with_capacity(monitoring_times.len());

    for (k, &t) in monitoring_times.iter().enumerate() {
        let idx = (t / dt).round() as usize;
        let remaining = &remaining_swaps[k];
        let npv = remaining.npv(path[idx].clone(), t, model);
        let mut exposures: Vec<f64> = to_vec(npv).into_iter().map(|x| x.max(0.0)).collect();

        let mean = exposures.iter().sum::<f64>() / exposures.len() as f64;
        exposures.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let q_idx = ((0.95 * exposures.len() as f64).ceil() as usize)
            .saturating_sub(1)
            .min(exposures.len() - 1);

        ee.push(mean);
        pfe_95.push(exposures[q_idx]);
    }

    ExposureProfile {
        times: monitoring_times.to_vec(),
        ee,
        pfe_95,
    }
}

/// CVA unilateral simple: `(1-R) * sum_i EE(t_i) * [S(t_{i-1}) - S(t_i)] * P(0,t_i)`, con
/// probabilidad de supervivencia `S(t) = exp(-hazard_rate * t)` (hazard rate plana) y
/// descuento a hoy vía la fórmula cerrada del modelo. Simplificación estándar de
/// prototipo: no usa la medida de riesgo neutral simulada para el descuento del CVA en
/// sí (solo para generar el perfil EE), consistente con PLAN.md §5.2 ("CVA unilateral
/// simple como primera métrica XVA end-to-end").
pub fn unilateral_cva<B: Backend<FloatElem = f64>>(
    profile: &ExposureProfile,
    model: &HullWhite1F<B>,
    r0: f64,
    hazard_rate: f64,
    recovery_rate: f64,
    device: &burn::tensor::Device<B>,
) -> f64 {
    let mut cva = 0.0;
    let mut prev_survival = 1.0;

    for (i, &t) in profile.times.iter().enumerate() {
        let survival = (-hazard_rate * t).exp();
        let default_prob = prev_survival - survival;
        let discount = model.zero_coupon_bond(scalar(r0, device), 0.0, t).into_scalar();
        cva += (1.0 - recovery_rate) * profile.ee[i] * default_prob * discount;
        prev_survival = survival;
    }
    cva
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::CpuBackend;

    type Device = burn::tensor::Device<CpuBackend>;

    fn reference_model(device: &Device) -> HullWhite1F<CpuBackend> {
        HullWhite1F::new(scalar(0.1, device), scalar(0.03, device), scalar(0.01, device))
    }

    fn par_swap(r0: f64, model: &HullWhite1F<CpuBackend>, device: &Device) -> IrSwap<CpuBackend> {
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0, 1.0, 1.0, 1.0, 1.0];
        let k = IrSwap::par_rate(scalar(r0, device), 0.0, &payment_times, &accruals, model);
        IrSwap {
            notional: scalar(1_000_000.0, device),
            fixed_rate: k,
            start: 0.0,
            payment_times,
            accruals,
        }
    }

    #[test]
    fn exposure_is_nonnegative_and_pfe_dominates_ee() {
        let device = Device::default();
        let model = reference_model(&device);
        let r0 = 0.02;
        let swap = par_swap(r0, &model, &device);
        let times = vec![0.0, 1.0, 2.0];
        let profile = expected_exposure_profile(&model, &swap, r0, &times, 5_000, 7, &device);

        for i in 0..times.len() {
            assert!(profile.ee[i] >= 0.0);
            assert!(profile.pfe_95[i] >= profile.ee[i]);
        }
    }

    #[test]
    fn exposure_at_time_zero_matches_deterministic_npv_exactly() {
        // En t=0 todas las trayectorias arrancan en r0: sin aleatoriedad, EE(0) debe
        // coincidir exactamente (salvo redondeo) con el NPV determinista.
        let device = Device::default();
        let model = reference_model(&device);
        let r0 = 0.02;
        let swap = par_swap(r0, &model, &device);
        let profile = expected_exposure_profile(&model, &swap, r0, &[0.0], 1_000, 11, &device);

        let deterministic = swap.npv(scalar(r0, &device), 0.0, &model).into_scalar().max(0.0);
        assert!((profile.ee[0] - deterministic).abs() < 1e-6);
        assert!((profile.pfe_95[0] - deterministic).abs() < 1e-6);
    }

    #[test]
    fn unilateral_cva_is_positive_for_nonzero_hazard_rate() {
        let device = Device::default();
        let model = reference_model(&device);
        let r0 = 0.02;
        let swap = par_swap(r0, &model, &device);
        let times = vec![0.0, 1.0, 2.0, 3.0];
        let profile = expected_exposure_profile(&model, &swap, r0, &times, 5_000, 13, &device);

        let cva = unilateral_cva(&profile, &model, r0, 0.02, 0.4, &device);
        assert!(cva > 0.0, "CVA debería ser positivo, got {cva}");
    }

    #[test]
    fn unilateral_cva_is_zero_when_hazard_rate_is_zero() {
        let device = Device::default();
        let model = reference_model(&device);
        let r0 = 0.02;
        let swap = par_swap(r0, &model, &device);
        let times = vec![0.0, 1.0, 2.0];
        let profile = expected_exposure_profile(&model, &swap, r0, &times, 2_000, 17, &device);

        let cva = unilateral_cva(&profile, &model, r0, 0.0, 0.4, &device);
        assert!(cva.abs() < 1e-9, "CVA con hazard=0 debería ser 0, got {cva}");
    }
}
