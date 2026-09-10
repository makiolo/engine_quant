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
use crate::models::hull_white_2f::HullWhite2F;
use crate::products::irs::IrSwap;
use burn::tensor::backend::Backend;
use burn::tensor::{Distribution, Tensor, TensorData};

fn scalar<B: Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

fn to_vec<B: Backend>(t: Tensor<B, 1>) -> Vec<f64> {
    t.into_data().to_vec::<f64>().unwrap()
}

/// Media (EE) y cuantil al 95% (PFE) de una muestra de exposiciones ya truncadas a `>= 0`
/// (un path Monte Carlo por elemento) -- extraído de `expected_exposure_profile` para que
/// `expected_exposure_profile_2f` (PLAN.md §7.16) lo reutilice sin duplicar la agregación
/// estadística, que no depende de cuántos factores tenga el modelo.
fn ee_pfe(mut exposures: Vec<f64>) -> (f64, f64) {
    let mean = exposures.iter().sum::<f64>() / exposures.len() as f64;
    exposures.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let q_idx = ((0.95 * exposures.len() as f64).ceil() as usize)
        .saturating_sub(1)
        .min(exposures.len() - 1);
    (mean, exposures[q_idx])
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
/// una malla de `n_steps` pasos hasta la última fecha de monitorización, y revalorando en
/// cada una el swap *restante* (`IrSwap::remaining_from`) analíticamente vía `IrSwap::npv`.
///
/// `n_steps` es explícito (PLAN.md §7.15, `PricingContext::n_steps`) — antes de esa fase se
/// calculaba internamente una malla semanal fija; ahora lo decide quien llama, para poder
/// controlar la granularidad/coste de la simulación caso a caso.
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
    n_steps: usize,
    n_paths: usize,
    seed: u64,
    device: &burn::tensor::Device<B>,
) -> ExposureProfile {
    assert!(!monitoring_times.is_empty(), "monitoring_times vacío");
    assert!(n_steps >= 1, "n_steps debe ser al menos 1");
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

    // Las fechas de monitorización se muestrean al índice de grid más cercano.
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
        let exposures: Vec<f64> = to_vec(npv).into_iter().map(|x| x.max(0.0)).collect();

        let (mean, q) = ee_pfe(exposures);
        ee.push(mean);
        pfe_95.push(q);
    }

    ExposureProfile {
        times: monitoring_times.to_vec(),
        ee,
        pfe_95,
    }
}

/// Núcleo del CVA unilateral simple, independiente del modelo: `(1-R) * sum_i EE(t_i) *
/// [S(t_{i-1}) - S(t_i)] * P(0,t_i)`, con probabilidad de supervivencia `S(t) =
/// exp(-hazard_rate * t)` (hazard rate plana) y `discount(t) = P(0,t)` provisto por quien
/// llama -- así `unilateral_cva`/`unilateral_cva_2f` (PLAN.md §7.16) comparten esta
/// agregación sin duplicarla, cada uno solo aporta cómo descuenta su propio modelo.
fn unilateral_cva_with_discount(
    profile: &ExposureProfile,
    hazard_rate: f64,
    recovery_rate: f64,
    discount: impl Fn(f64) -> f64,
) -> f64 {
    let mut cva = 0.0;
    let mut prev_survival = 1.0;

    for (i, &t) in profile.times.iter().enumerate() {
        let survival = (-hazard_rate * t).exp();
        let default_prob = prev_survival - survival;
        cva += (1.0 - recovery_rate) * profile.ee[i] * default_prob * discount(t);
        prev_survival = survival;
    }
    cva
}

/// CVA unilateral simple bajo `HullWhite1F` (ver `unilateral_cva_with_discount` para la
/// fórmula). Simplificación estándar de prototipo: no usa la medida de riesgo neutral
/// simulada para el descuento del CVA en sí (solo para generar el perfil EE), consistente
/// con PLAN.md §5.2 ("CVA unilateral simple como primera métrica XVA end-to-end").
pub fn unilateral_cva<B: Backend<FloatElem = f64>>(
    profile: &ExposureProfile,
    model: &HullWhite1F<B>,
    r0: f64,
    hazard_rate: f64,
    recovery_rate: f64,
    device: &burn::tensor::Device<B>,
) -> f64 {
    unilateral_cva_with_discount(profile, hazard_rate, recovery_rate, |t| {
        model.zero_coupon_bond(scalar(r0, device), 0.0, t).into_scalar()
    })
}

/// Perfil de exposición esperada (EE) y PFE al 95% bajo `HullWhite2F` (PLAN.md §7.16),
/// mismo algoritmo que `expected_exposure_profile` (Monte Carlo vectorizado + revaloración
/// analítica en cada fecha de reseteo vía `IrSwap::npv`, genérico gracias a
/// `crate::models::ShortRateModel`) -- la única diferencia real es que aquí se simulan dos
/// factores correlacionados (`HullWhite2F::simulate_path`) en vez de uno, y el estado
/// inicial `(x_0, y_0) = (0, 0)` no es un parámetro (el nivel de tipos ya vive en
/// `model.r0`, ver docs de `HullWhite2F`), a diferencia del `r0` explícito de la versión de
/// 1 factor.
#[allow(clippy::too_many_arguments)]
pub fn expected_exposure_profile_2f<B: Backend<FloatElem = f64>>(
    model: &HullWhite2F<B>,
    swap: &IrSwap<B>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
    device: &burn::tensor::Device<B>,
) -> ExposureProfile {
    assert!(!monitoring_times.is_empty(), "monitoring_times vacío");
    assert!(n_steps >= 1, "n_steps debe ser al menos 1");
    for &t in monitoring_times {
        assert!(
            swap.is_reset_date(t),
            "t={t} no es una fecha de reseteo válida del swap"
        );
    }
    let t_max = monitoring_times.iter().cloned().fold(f64::MIN, f64::max);

    let remaining_swaps: Vec<IrSwap<B>> =
        monitoring_times.iter().map(|&t| swap.remaining_from(t)).collect();

    if t_max <= 0.0 {
        let state0 = (scalar(0.0, device), scalar(0.0, device));
        let npv = remaining_swaps[0]
            .npv(state0, monitoring_times[0], model)
            .into_scalar()
            .max(0.0);
        return ExposureProfile {
            times: monitoring_times.to_vec(),
            ee: vec![npv],
            pfe_95: vec![npv],
        };
    }

    let dt = t_max / n_steps as f64;

    B::seed(device, seed);
    let shocks_x: Vec<Tensor<B, 1>> = (0..n_steps)
        .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), device))
        .collect();
    let shocks_y: Vec<Tensor<B, 1>> = (0..n_steps)
        .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), device))
        .collect();
    let path = model.simulate_path(dt, &shocks_x, &shocks_y);

    let mut ee = Vec::with_capacity(monitoring_times.len());
    let mut pfe_95 = Vec::with_capacity(monitoring_times.len());

    for (k, &t) in monitoring_times.iter().enumerate() {
        let idx = (t / dt).round() as usize;
        let remaining = &remaining_swaps[k];
        let npv = remaining.npv(path[idx].clone(), t, model);
        let exposures: Vec<f64> = to_vec(npv).into_iter().map(|x| x.max(0.0)).collect();

        let (mean, q) = ee_pfe(exposures);
        ee.push(mean);
        pfe_95.push(q);
    }

    ExposureProfile {
        times: monitoring_times.to_vec(),
        ee,
        pfe_95,
    }
}

/// CVA unilateral simple bajo `HullWhite2F` (ver `unilateral_cva_with_discount`): el
/// descuento a hoy usa el estado inicial `(0, 0)` del modelo en vez de un `r0` explícito
/// (mismo motivo que `expected_exposure_profile_2f`).
pub fn unilateral_cva_2f<B: Backend<FloatElem = f64>>(
    profile: &ExposureProfile,
    model: &HullWhite2F<B>,
    hazard_rate: f64,
    recovery_rate: f64,
    device: &burn::tensor::Device<B>,
) -> f64 {
    unilateral_cva_with_discount(profile, hazard_rate, recovery_rate, |t| {
        model
            .zero_coupon_bond(scalar(0.0, device), scalar(0.0, device), 0.0, t)
            .into_scalar()
    })
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
        let profile = expected_exposure_profile(&model, &swap, r0, &times, 104, 5_000, 7, &device);

        for i in 0..times.len() {
            assert!(profile.ee[i] >= 0.0);
            assert!(profile.pfe_95[i] >= profile.ee[i]);
        }
    }

    #[test]
    fn exposure_at_time_zero_matches_deterministic_npv_exactly() {
        // En t=0 todas las trayectorias arrancan en r0: sin aleatoriedad, EE(0) debe
        // coincidir exactamente (salvo redondeo) con el NPV determinista. n_steps es
        // irrelevante aquí (t_max=0 devuelve antes de tocar la malla).
        let device = Device::default();
        let model = reference_model(&device);
        let r0 = 0.02;
        let swap = par_swap(r0, &model, &device);
        let profile = expected_exposure_profile(&model, &swap, r0, &[0.0], 1, 1_000, 11, &device);

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
        let profile = expected_exposure_profile(&model, &swap, r0, &times, 156, 5_000, 13, &device);

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
        let profile = expected_exposure_profile(&model, &swap, r0, &times, 104, 2_000, 17, &device);

        let cva = unilateral_cva(&profile, &model, r0, 0.0, 0.4, &device);
        assert!(cva.abs() < 1e-9, "CVA con hazard=0 debería ser 0, got {cva}");
    }

    fn reference_model_2f(device: &Device) -> HullWhite2F<CpuBackend> {
        HullWhite2F::new(
            scalar(0.1, device),
            scalar(0.2, device),
            scalar(0.01, device),
            scalar(0.012, device),
            -0.7,
            scalar(0.03, device),
        )
    }

    fn par_swap_2f(model: &HullWhite2F<CpuBackend>, device: &Device) -> IrSwap<CpuBackend> {
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0, 1.0, 1.0, 1.0, 1.0];
        let state0 = (scalar(0.0, device), scalar(0.0, device));
        let k = IrSwap::par_rate(state0, 0.0, &payment_times, &accruals, model);
        IrSwap {
            notional: scalar(1_000_000.0, device),
            fixed_rate: k,
            start: 0.0,
            payment_times,
            accruals,
        }
    }

    #[test]
    fn exposure_2f_is_nonnegative_and_pfe_dominates_ee() {
        let device = Device::default();
        let model = reference_model_2f(&device);
        let swap = par_swap_2f(&model, &device);
        let times = vec![0.0, 1.0, 2.0];
        let profile = expected_exposure_profile_2f(&model, &swap, &times, 104, 5_000, 7, &device);

        for i in 0..times.len() {
            assert!(profile.ee[i] >= 0.0);
            assert!(profile.pfe_95[i] >= profile.ee[i]);
        }
    }

    #[test]
    fn exposure_2f_at_time_zero_matches_deterministic_npv_exactly() {
        let device = Device::default();
        let model = reference_model_2f(&device);
        let swap = par_swap_2f(&model, &device);
        let profile = expected_exposure_profile_2f(&model, &swap, &[0.0], 1, 1_000, 11, &device);

        let state0 = (scalar(0.0, &device), scalar(0.0, &device));
        let deterministic = swap.npv(state0, 0.0, &model).into_scalar().max(0.0);
        assert!((profile.ee[0] - deterministic).abs() < 1e-6);
        assert!((profile.pfe_95[0] - deterministic).abs() < 1e-6);
    }

    #[test]
    fn unilateral_cva_2f_is_positive_for_nonzero_hazard_rate() {
        let device = Device::default();
        let model = reference_model_2f(&device);
        let swap = par_swap_2f(&model, &device);
        let times = vec![0.0, 1.0, 2.0, 3.0];
        let profile = expected_exposure_profile_2f(&model, &swap, &times, 156, 5_000, 13, &device);

        let cva = unilateral_cva_2f(&profile, &model, 0.02, 0.4, &device);
        assert!(cva > 0.0, "CVA debería ser positivo, got {cva}");
    }

    #[test]
    fn unilateral_cva_2f_is_zero_when_hazard_rate_is_zero() {
        let device = Device::default();
        let model = reference_model_2f(&device);
        let swap = par_swap_2f(&model, &device);
        let times = vec![0.0, 1.0, 2.0];
        let profile = expected_exposure_profile_2f(&model, &swap, &times, 104, 2_000, 17, &device);

        let cva = unilateral_cva_2f(&profile, &model, 0.0, 0.4, &device);
        assert!(cva.abs() < 1e-9, "CVA con hazard=0 debería ser 0, got {cva}");
    }
}
