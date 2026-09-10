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

/// Datos planos (orden fila-mayor, PLAN.md §7.19) de un `Tensor<B,2>` forma `[n_paths,
/// n_trades]` -- usado por las versiones de lote de `expected_exposure_profile*` para extraer
/// la columna de cada trade (`flat[path * n_trades + trade]`) antes de reducir con `ee_pfe`.
fn to_vec2<B: Backend>(t: Tensor<B, 2>) -> Vec<f64> {
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

/// Reduce un tensor de NPVs de lote ya aplanado fila-mayor (forma lógica `[n_filas,
/// n_trades]`, PLAN.md §7.19) a `(EE, PFE95)` por trade -- compartido por
/// `expected_exposure_profile_batch`/`_2f_batch`, igual que `ee_pfe` (no-lote) ya se comparte
/// entre las versiones de 1 y 2 factores. El cuantil (PFE95) no tiene primitiva vectorizada en
/// Burn: cada columna se ordena en Rust puro, igual que ya hacía la versión no-lote.
///
/// `n_filas` se deriva de `flat_npvs.len() / n_trades`, **no** se recibe como parámetro: en
/// `t=0` bajo `HullWhite1F`, `path[0]` es literalmente `r0` sin difundir (forma `[1]`, ver
/// `HullWhite1F::simulate_path`), así que esa fecha concreta tiene una sola "fila" aunque
/// `n_paths` sea mayor -- mismo caso límite que ya explota la versión no-lote (un único NPV
/// determinista hace de EE=PFE95 en `t=0`), aquí generalizado a lote sin caso especial.
fn ee_pfe_per_trade(flat_npvs: &[f64], n_trades: usize) -> Vec<(f64, f64)> {
    let n_filas = flat_npvs.len() / n_trades;
    (0..n_trades)
        .map(|trade| {
            let exposures: Vec<f64> = (0..n_filas).map(|p| flat_npvs[p * n_trades + trade].max(0.0)).collect();
            ee_pfe(exposures)
        })
        .collect()
}

/// Equivalente de lote de `expected_exposure_profile` (PLAN.md §7.19): `swap` es un lote
/// homogéneo (`swap.notional`/`swap.fixed_rate` de forma `[n_trades]`, ver
/// `IrSwap::npv_batch_over_paths`) que comparte calendario y modelo/mercado. Simula el tipo
/// corto **una sola vez** (todos los trades del lote comparten el mismo escenario Monte Carlo,
/// lo matemáticamente correcto para exposición de cartera) y revalora todos los trades a la vez
/// en cada fecha de monitorización. Devuelve un `ExposureProfile` por trade -- mismo tipo que
/// la versión no-lote, cero structs nuevos.
#[allow(clippy::too_many_arguments)]
pub fn expected_exposure_profile_batch<B: Backend<FloatElem = f64>>(
    model: &HullWhite1F<B>,
    swap: &IrSwap<B>,
    r0: f64,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
    device: &burn::tensor::Device<B>,
) -> Vec<ExposureProfile> {
    assert!(!monitoring_times.is_empty(), "monitoring_times vacío");
    assert!(n_steps >= 1, "n_steps debe ser al menos 1");
    for &t in monitoring_times {
        assert!(
            swap.is_reset_date(t),
            "t={t} no es una fecha de reseteo válida del swap"
        );
    }
    let n_trades = swap.batch_len();
    let t_max = monitoring_times.iter().cloned().fold(f64::MIN, f64::max);

    let remaining_swaps: Vec<IrSwap<B>> =
        monitoring_times.iter().map(|&t| swap.remaining_from(t)).collect();

    if t_max <= 0.0 {
        let flat = to_vec2(remaining_swaps[0].npv_batch_over_paths(scalar(r0, device), monitoring_times[0], model));
        return (0..n_trades)
            .map(|trade| {
                let npv = flat[trade].max(0.0);
                ExposureProfile {
                    times: monitoring_times.to_vec(),
                    ee: vec![npv],
                    pfe_95: vec![npv],
                }
            })
            .collect();
    }

    let dt = t_max / n_steps as f64;

    B::seed(device, seed);
    let shocks: Vec<Tensor<B, 1>> = (0..n_steps)
        .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), device))
        .collect();
    let path = model.simulate_path(scalar(r0, device), dt, &shocks);

    let mut ee_per_trade: Vec<Vec<f64>> = vec![Vec::with_capacity(monitoring_times.len()); n_trades];
    let mut pfe_per_trade: Vec<Vec<f64>> = vec![Vec::with_capacity(monitoring_times.len()); n_trades];

    for (k, &t) in monitoring_times.iter().enumerate() {
        let idx = (t / dt).round() as usize;
        let remaining = &remaining_swaps[k];
        let flat = to_vec2(remaining.npv_batch_over_paths(path[idx].clone(), t, model));
        for (trade, (mean, q)) in ee_pfe_per_trade(&flat, n_trades).into_iter().enumerate() {
            ee_per_trade[trade].push(mean);
            pfe_per_trade[trade].push(q);
        }
    }

    (0..n_trades)
        .map(|trade| ExposureProfile {
            times: monitoring_times.to_vec(),
            ee: std::mem::take(&mut ee_per_trade[trade]),
            pfe_95: std::mem::take(&mut pfe_per_trade[trade]),
        })
        .collect()
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

/// Equivalente de lote de `unilateral_cva` (PLAN.md §7.19): un CVA por perfil de `profiles`
/// (uno por trade, ver `expected_exposure_profile_batch`). `hazard_rate`/`recovery_rate` son
/// compartidos por todo el lote (vienen del mismo `Market`) -- cada perfil tiene como mucho
/// unas pocas fechas de monitorización, así que el bucle es Rust puro y barato, sin tensores:
/// mismo motivo por el que la versión no-lote (`unilateral_cva_with_discount`) tampoco los usa.
pub fn unilateral_cva_batch<B: Backend<FloatElem = f64>>(
    profiles: &[ExposureProfile],
    model: &HullWhite1F<B>,
    r0: f64,
    hazard_rate: f64,
    recovery_rate: f64,
    device: &burn::tensor::Device<B>,
) -> Vec<f64> {
    profiles
        .iter()
        .map(|profile| unilateral_cva(profile, model, r0, hazard_rate, recovery_rate, device))
        .collect()
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

/// Equivalente de lote de `expected_exposure_profile_2f` (PLAN.md §7.19): mismo algoritmo que
/// `expected_exposure_profile_batch` (simula una vez, revalora el lote entero por fecha de
/// monitorización vía `IrSwap::npv_batch_over_paths`) -- la única diferencia real es simular
/// dos factores correlacionados en vez de uno, igual que ya distingue
/// `expected_exposure_profile_2f` de su versión de 1 factor.
#[allow(clippy::too_many_arguments)]
pub fn expected_exposure_profile_2f_batch<B: Backend<FloatElem = f64>>(
    model: &HullWhite2F<B>,
    swap: &IrSwap<B>,
    monitoring_times: &[f64],
    n_steps: usize,
    n_paths: usize,
    seed: u64,
    device: &burn::tensor::Device<B>,
) -> Vec<ExposureProfile> {
    assert!(!monitoring_times.is_empty(), "monitoring_times vacío");
    assert!(n_steps >= 1, "n_steps debe ser al menos 1");
    for &t in monitoring_times {
        assert!(
            swap.is_reset_date(t),
            "t={t} no es una fecha de reseteo válida del swap"
        );
    }
    let n_trades = swap.batch_len();
    let t_max = monitoring_times.iter().cloned().fold(f64::MIN, f64::max);

    let remaining_swaps: Vec<IrSwap<B>> =
        monitoring_times.iter().map(|&t| swap.remaining_from(t)).collect();

    if t_max <= 0.0 {
        let state0 = (scalar(0.0, device), scalar(0.0, device));
        let flat = to_vec2(remaining_swaps[0].npv_batch_over_paths(state0, monitoring_times[0], model));
        return (0..n_trades)
            .map(|trade| {
                let npv = flat[trade].max(0.0);
                ExposureProfile {
                    times: monitoring_times.to_vec(),
                    ee: vec![npv],
                    pfe_95: vec![npv],
                }
            })
            .collect();
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

    let mut ee_per_trade: Vec<Vec<f64>> = vec![Vec::with_capacity(monitoring_times.len()); n_trades];
    let mut pfe_per_trade: Vec<Vec<f64>> = vec![Vec::with_capacity(monitoring_times.len()); n_trades];

    for (k, &t) in monitoring_times.iter().enumerate() {
        let idx = (t / dt).round() as usize;
        let remaining = &remaining_swaps[k];
        let flat = to_vec2(remaining.npv_batch_over_paths(path[idx].clone(), t, model));
        for (trade, (mean, q)) in ee_pfe_per_trade(&flat, n_trades).into_iter().enumerate() {
            ee_per_trade[trade].push(mean);
            pfe_per_trade[trade].push(q);
        }
    }

    (0..n_trades)
        .map(|trade| ExposureProfile {
            times: monitoring_times.to_vec(),
            ee: std::mem::take(&mut ee_per_trade[trade]),
            pfe_95: std::mem::take(&mut pfe_per_trade[trade]),
        })
        .collect()
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

/// Equivalente de lote de `unilateral_cva_2f` (PLAN.md §7.19), mismo motivo/patrón que
/// `unilateral_cva_batch`.
pub fn unilateral_cva_2f_batch<B: Backend<FloatElem = f64>>(
    profiles: &[ExposureProfile],
    model: &HullWhite2F<B>,
    hazard_rate: f64,
    recovery_rate: f64,
    device: &burn::tensor::Device<B>,
) -> Vec<f64> {
    profiles
        .iter()
        .map(|profile| unilateral_cva_2f(profile, model, hazard_rate, recovery_rate, device))
        .collect()
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
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
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
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
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
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
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
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
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
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
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
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let model = reference_model_2f(&device);
        let swap = par_swap_2f(&model, &device);
        let times = vec![0.0, 1.0, 2.0];
        let profile = expected_exposure_profile_2f(&model, &swap, &times, 104, 2_000, 17, &device);

        let cva = unilateral_cva_2f(&profile, &model, 0.0, 0.4, &device);
        assert!(cva.abs() < 1e-9, "CVA con hazard=0 debería ser 0, got {cva}");
    }

    fn batch_swap(
        notionals: &[f64],
        fixed_rates: &[f64],
        payment_times: Vec<f64>,
        accruals: Vec<f64>,
        device: &Device,
    ) -> IrSwap<CpuBackend> {
        IrSwap {
            notional: Tensor::from_data(TensorData::from(notionals), device),
            fixed_rate: Tensor::from_data(TensorData::from(fixed_rates), device),
            start: 0.0,
            payment_times,
            accruals,
        }
    }

    /// PLAN.md §7.19: `expected_exposure_profile_batch` debe coincidir, trade a trade, con
    /// `IrSwap::npv` (escalar) evaluado sobre EXACTAMENTE el mismo camino Monte Carlo. La
    /// comparación se hace contra un camino simulado *una sola vez en este test* -- no contra
    /// una segunda llamada a `expected_exposure_profile` con la misma seed -- porque
    /// `NdArray::seed` (burn-ndarray) usa un `Mutex` **global de proceso**, no un RNG por hilo:
    /// dos llamadas independientes a una función que reseeda no son reproducibles bit a bit si
    /// el binario de test corre en paralelo (el modo por defecto de `cargo test`), aunque usen
    /// la misma seed. Sembrar una única vez en este test evita esa carrera por construcción.
    #[test]
    fn expected_exposure_profile_batch_matches_a_loop_of_scalar_calls() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let model = reference_model(&device);
        let r0 = 0.02;
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0, 1.0, 1.0, 1.0, 1.0];
        let notionals = [1_000_000.0, 2_000_000.0, 500_000.0];
        let fixed_rates = [0.018, 0.02, 0.022];
        let times = vec![0.0, 1.0, 2.0];
        let (seed, n_paths, n_steps) = (7, 2_000, 104);

        let swap = batch_swap(&notionals, &fixed_rates, payment_times.clone(), accruals.clone(), &device);
        let batch_profiles = expected_exposure_profile_batch(&model, &swap, r0, &times, n_steps, n_paths, seed, &device);
        assert_eq!(batch_profiles.len(), notionals.len());

        let t_max = times.iter().cloned().fold(f64::MIN, f64::max);
        let dt = t_max / n_steps as f64;
        CpuBackend::seed(&device, seed);
        let shocks: Vec<Tensor<CpuBackend, 1>> = (0..n_steps)
            .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), &device))
            .collect();
        let path = model.simulate_path(scalar(r0, &device), dt, &shocks);

        for (i, (&notional, &fixed_rate)) in notionals.iter().zip(fixed_rates.iter()).enumerate() {
            let scalar_swap = IrSwap {
                notional: scalar(notional, &device),
                fixed_rate: scalar(fixed_rate, &device),
                start: 0.0,
                payment_times: payment_times.clone(),
                accruals: accruals.clone(),
            };
            for (k, &t) in times.iter().enumerate() {
                let idx = (t / dt).round() as usize;
                let remaining = scalar_swap.remaining_from(t);
                let exposures: Vec<f64> = to_vec(remaining.npv(path[idx].clone(), t, &model))
                    .into_iter()
                    .map(|x| x.max(0.0))
                    .collect();
                let (mean, q) = ee_pfe(exposures);
                assert!(
                    (batch_profiles[i].ee[k] - mean).abs() < 1e-9,
                    "trade {i} fecha {k}: EE lote={} escalar={}",
                    batch_profiles[i].ee[k],
                    mean
                );
                assert!(
                    (batch_profiles[i].pfe_95[k] - q).abs() < 1e-9,
                    "trade {i} fecha {k}: PFE95 lote={} escalar={}",
                    batch_profiles[i].pfe_95[k],
                    q
                );
            }
        }
    }

    /// El CVA en sí no usa Monte Carlo (es una suma cerrada sobre un `ExposureProfile` ya
    /// calculado, ver `unilateral_cva_with_discount`): perfiles fabricados a mano bastan para
    /// probar que `unilateral_cva_batch` reproduce `unilateral_cva` por perfil, sin ningún
    /// `B::seed` de por medio (y por tanto sin la carrera documentada arriba).
    #[test]
    fn unilateral_cva_batch_matches_a_loop_of_scalar_calls() {
        let device = Device::default();
        let model = reference_model(&device);
        let r0 = 0.02;
        let profiles = vec![
            ExposureProfile {
                times: vec![0.0, 1.0, 2.0],
                ee: vec![0.0, 12_000.0, 9_500.0],
                pfe_95: vec![0.0, 48_000.0, 38_000.0],
            },
            ExposureProfile {
                times: vec![0.0, 1.0, 2.0],
                ee: vec![0.0, 24_000.0, 19_000.0],
                pfe_95: vec![0.0, 96_000.0, 76_000.0],
            },
        ];

        let batch_cva = unilateral_cva_batch(&profiles, &model, r0, 0.02, 0.4, &device);
        assert_eq!(batch_cva.len(), profiles.len());

        for (i, profile) in profiles.iter().enumerate() {
            let scalar_cva = unilateral_cva(profile, &model, r0, 0.02, 0.4, &device);
            assert!(
                (batch_cva[i] - scalar_cva).abs() < 1e-12,
                "trade {i}: CVA lote={} escalar={}",
                batch_cva[i],
                scalar_cva
            );
        }
    }

    /// Equivalente 2F de `expected_exposure_profile_batch_matches_a_loop_of_scalar_calls` (ver
    /// esa prueba para el porqué de sembrar una única vez en todo el test).
    #[test]
    fn expected_exposure_profile_2f_batch_matches_a_loop_of_scalar_calls() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let model = reference_model_2f(&device);
        let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = vec![1.0, 1.0, 1.0, 1.0, 1.0];
        let notionals = [1_000_000.0, 2_000_000.0, 500_000.0];
        let fixed_rates = [0.018, 0.02, 0.022];
        let times = vec![0.0, 1.0, 2.0];
        let (seed, n_paths, n_steps) = (7, 2_000, 104);

        let swap = batch_swap(&notionals, &fixed_rates, payment_times.clone(), accruals.clone(), &device);
        let batch_profiles = expected_exposure_profile_2f_batch(&model, &swap, &times, n_steps, n_paths, seed, &device);
        assert_eq!(batch_profiles.len(), notionals.len());

        let t_max = times.iter().cloned().fold(f64::MIN, f64::max);
        let dt = t_max / n_steps as f64;
        CpuBackend::seed(&device, seed);
        let shocks_x: Vec<Tensor<CpuBackend, 1>> = (0..n_steps)
            .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), &device))
            .collect();
        let shocks_y: Vec<Tensor<CpuBackend, 1>> = (0..n_steps)
            .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), &device))
            .collect();
        let path = model.simulate_path(dt, &shocks_x, &shocks_y);

        for (i, (&notional, &fixed_rate)) in notionals.iter().zip(fixed_rates.iter()).enumerate() {
            let scalar_swap = IrSwap {
                notional: scalar(notional, &device),
                fixed_rate: scalar(fixed_rate, &device),
                start: 0.0,
                payment_times: payment_times.clone(),
                accruals: accruals.clone(),
            };
            for (k, &t) in times.iter().enumerate() {
                let idx = (t / dt).round() as usize;
                let remaining = scalar_swap.remaining_from(t);
                let exposures: Vec<f64> = to_vec(remaining.npv(path[idx].clone(), t, &model))
                    .into_iter()
                    .map(|x| x.max(0.0))
                    .collect();
                let (mean, q) = ee_pfe(exposures);
                assert!(
                    (batch_profiles[i].ee[k] - mean).abs() < 1e-9,
                    "trade {i} fecha {k}: EE lote={} escalar={}",
                    batch_profiles[i].ee[k],
                    mean
                );
                assert!(
                    (batch_profiles[i].pfe_95[k] - q).abs() < 1e-9,
                    "trade {i} fecha {k}: PFE95 lote={} escalar={}",
                    batch_profiles[i].pfe_95[k],
                    q
                );
            }
        }
    }

    /// Ver `unilateral_cva_batch_matches_a_loop_of_scalar_calls`: el CVA es determinista dado
    /// un perfil, sin `B::seed` de por medio.
    #[test]
    fn unilateral_cva_2f_batch_matches_a_loop_of_scalar_calls() {
        let device = Device::default();
        let model = reference_model_2f(&device);
        let profiles = vec![
            ExposureProfile {
                times: vec![0.0, 1.0, 2.0],
                ee: vec![0.0, 12_000.0, 9_500.0],
                pfe_95: vec![0.0, 48_000.0, 38_000.0],
            },
            ExposureProfile {
                times: vec![0.0, 1.0, 2.0],
                ee: vec![0.0, 24_000.0, 19_000.0],
                pfe_95: vec![0.0, 96_000.0, 76_000.0],
            },
        ];

        let batch_cva = unilateral_cva_2f_batch(&profiles, &model, 0.02, 0.4, &device);
        assert_eq!(batch_cva.len(), profiles.len());

        for (i, profile) in profiles.iter().enumerate() {
            let scalar_cva = unilateral_cva_2f(profile, &model, 0.02, 0.4, &device);
            assert!(
                (batch_cva[i] - scalar_cva).abs() < 1e-12,
                "trade {i}: CVA lote={} escalar={}",
                batch_cva[i],
                scalar_cva
            );
        }
    }
}
