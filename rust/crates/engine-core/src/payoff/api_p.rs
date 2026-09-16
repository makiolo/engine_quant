//! Orquestacion de medidas bajo P de un `CompiledPayoff` (PLAN_PRODUCTS.md §12 Fase 7:
//! "configuracion fisica separada", "Forecast, HitProbabilityP, distribucion de P&L y expected
//! shortfall de estrategia"). Estructura deliberadamente paralela a `crate::payoff::api` (Q):
//! misma compilacion (`compile::compile`), mismo interprete pathwise (`eval::evaluate_with_events_seeded`),
//! mismo `SinglePath`/`bridge_seed_for_path` -- la unica diferencia real es que las rutas se
//! simulan bajo `crate::models::gbm_p::GbmP` (drift fisico `mu`) en vez de `crate::models::gbm::Gbm`
//! (drift `r-q`), y que ninguna de estas tres funciones descuenta a valor presente: bajo P no
//! existe un numerario libre de riesgo canonico -- descontar aqui inventaria una curva fisica que
//! el documento no pide (§6: "P se reserva para forecast, probabilidades reales, stress y P&L").
//! `Forecast`/P&L reportan por tanto CANTIDADES NO DESCONTADAS en la fecha de pago de cada
//! cashflow, tal cual las produce el ledger -- quien consuma el resultado descuenta con su propia
//! curva/tasa fisica si lo necesita (mismo principio que "el visitor de valoracion decide como
//! descontar", §14).
//!
//! Preflight identico en espiritu al de `api::price_payoff_gbm_q` (observable no generado,
//! `n_paths > 0`) reutilizando `check_single_observable` -- esa funcion no depende de la medida,
//! solo del `CompiledPayoff` y del nombre de observable que el modelo genera.

use crate::backend::CpuBackend;
use crate::mc::{self, McEstimate};
use crate::models::gbm_p::GbmP;
use crate::payoff::api::{check_single_observable, LocalHessianEstimate};
use crate::payoff::compile::compile;
use crate::payoff::dual::Dual;
use crate::payoff::eval::{evaluate_with_events_seeded, resolve_trigger_states, ObservablePath};
use crate::payoff::ir::CompiledPayoff;
use crate::payoff::lrm;
use crate::payoff::sensitivity::{contains_exercise, evaluate_dual, GbmDualPath, GbmPGreek};
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

fn scalar(value: f64, device: &burn::tensor::Device<CpuBackend>) -> Tensor<CpuBackend, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Identico a `api::SinglePath` (privado a ese modulo, de ahi la duplicacion): una unica ruta ya
/// simulada, un unico observable (slot 0).
struct SinglePath<'a> {
    times: &'a [f64],
    values: &'a [f64],
    sigma: f64,
}

impl ObservablePath for SinglePath<'_> {
    fn value_at(&self, _slot: usize, time: f64) -> f64 {
        let idx = self
            .times
            .iter()
            .position(|&t| (t - time).abs() < 1e-9)
            .unwrap_or_else(|| {
                panic!("payoff: tiempo {time} no simulado por el modelo (times={:?})", self.times)
            });
        self.values[idx]
    }

    fn volatility(&self, _slot: usize) -> f64 {
        self.sigma
    }
}

/// Identico a `api::bridge_seed_for_path` (mismo motivo de independencia del RNG de Burn, ver el
/// doc-comment de ese modulo).
fn bridge_seed_for_path(seed: u64, path_idx: usize) -> u64 {
    seed.wrapping_add((path_idx as u64).wrapping_add(1).wrapping_mul(0x9E37_79B9_7F4A_7C15))
}

fn simulate_gbm_p_columns_at(
    times: &[f64],
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Vec<Vec<f64>> {
    let device = burn::tensor::Device::<CpuBackend>::default();
    CpuBackend::seed(&device, seed);
    let model = GbmP::new(scalar(s0, &device), scalar(mu, &device), scalar(sigma, &device));
    let simulated = model.simulate_at_times(times, n_paths as usize, &device);
    simulated.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect()
}

fn simulate_gbm_p_columns(
    payoff: &CompiledPayoff,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<(Vec<f64>, Vec<Vec<f64>>), String> {
    let times = payoff.required_times();
    if times.is_empty() {
        return Err(
            "payoff: el contrato no depende de ningun instante de mercado (nada que simular bajo P)".to_string(),
        );
    }
    let columns = simulate_gbm_p_columns_at(&times, s0, mu, sigma, n_paths, seed);
    Ok((times, columns))
}

/// Un cashflow indicativo por ruta (no descontado, ver el doc-comment del modulo), mas -- para
/// `hit_probability_gbm_p` -- si `event_slot` ocurrio en esa ruta.
fn simulate_forecast_samples(
    payoff: &CompiledPayoff,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<(Vec<f64>, Vec<Vec<bool>>), String> {
    let (times, columns) = simulate_gbm_p_columns(payoff, s0, mu, sigma, n_paths, seed)?;
    let n_paths_usize = n_paths as usize;

    let mut pnl_samples = Vec::with_capacity(n_paths_usize);
    let mut hit_by_event: Vec<Vec<bool>> = vec![Vec::with_capacity(n_paths_usize); payoff.event_slots.len()];
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &times, values: &values, sigma };
        let (ledger, events) = evaluate_with_events_seeded(payoff, &path, bridge_seed_for_path(seed, path_idx));
        let total: f64 = ledger.iter().map(|cf| cf.amount).sum();
        pnl_samples.push(total);
        for (slot, outcome) in events.iter().enumerate() {
            hit_by_event[slot].push(outcome.occurred);
        }
    }
    Ok((pnl_samples, hit_by_event))
}

/// "Forecast" (PLAN_PRODUCTS.md §12 Fase 7): valor esperado bajo P, NO descontado (ver el
/// doc-comment del modulo), de la suma de cashflows de `spec_json` bajo GBM fisico
/// (`s0`/`mu`/`sigma`), mas error estandar e intervalo de confianza del estimador Monte Carlo.
/// `observable` es, igual que en `api::price_payoff_gbm_q`, el UNICO nombre de observable que
/// este `GbmP` genera -- preflight identico (`Err` antes de simular una sola ruta si el contrato
/// referencia otro nombre o `n_paths == 0`).
pub fn forecast_gbm_p(
    spec_json: &str,
    observable: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let (pnl_samples, _hit_by_event) = simulate_forecast_samples(&payoff, s0, mu, sigma, n_paths, seed)?;
    Ok(mc::aggregate(&pnl_samples, mc::Z_95))
}

/// Probabilidad bajo P de que el evento `event` (un `Trigger` de `spec_json`) dispare en la ruta
/// (PLAN_PRODUCTS.md §12 Fase 7: "HitProbabilityP"), estimada por Monte Carlo bajo GBM fisico --
/// misma agregacion que `api::hit_probability_gbm_q`, unicamente las rutas cambian de medida.
#[allow(clippy::too_many_arguments)]
pub fn hit_probability_gbm_p(
    spec_json: &str,
    event: &str,
    observable: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let event_slot = payoff.event_slots.iter().position(|e| e == event).ok_or_else(|| {
        format!(
            "payoff: el evento '{event}' no existe en este contrato (eventos declarados: {:?})",
            payoff.event_slots
        )
    })?;
    let (_pnl_samples, hit_by_event) = simulate_forecast_samples(&payoff, s0, mu, sigma, n_paths, seed)?;
    let indicators: Vec<f64> =
        hit_by_event[event_slot].iter().map(|&occurred| if occurred { 1.0 } else { 0.0 }).collect();
    Ok(mc::aggregate(&indicators, mc::Z_95))
}

/// Distribucion de P&L de una estrategia bajo P (PLAN_PRODUCTS.md §12 Fase 7: "distribucion de
/// P&L y expected shortfall de estrategia") -- media/error estandar (identicos a `forecast_gbm_p`,
/// misma muestra) mas Value-at-Risk y Expected Shortfall al nivel `confidence` (p.ej. `0.95`).
///
/// Convencion de signo: `pnl` es la suma de cashflows tal cual la produce el ledger (positivo =
/// ganancia). `var` y `es` se reportan como PERDIDAS POSITIVAS (la convencion de riesgo habitual,
/// no la de P&L): `var = -cuantil_{1-confidence}(pnl)`, `es = -media(pnl | pnl <= cuantil)`. Con
/// esta convencion `es >= var` siempre (Expected Shortfall promedia el propio VaR y todo lo peor
/// que el, nunca puede ser una perdida menor).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PnlDistribution {
    pub mean: f64,
    pub std_error: f64,
    pub var: f64,
    pub es: f64,
    pub n_paths: u64,
}

#[allow(clippy::too_many_arguments)]
pub fn pnl_distribution_gbm_p(
    spec_json: &str,
    observable: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
    confidence: f64,
) -> Result<PnlDistribution, String> {
    if !(0.0..1.0).contains(&confidence) {
        return Err(format!("payoff: confidence debe estar en [0,1) (recibido {confidence})"));
    }
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let (mut pnl_samples, _hit_by_event) = simulate_forecast_samples(&payoff, s0, mu, sigma, n_paths, seed)?;

    let estimate = mc::aggregate(&pnl_samples, mc::Z_95);

    pnl_samples.sort_by(|a, b| a.partial_cmp(b).expect("payoff: pnl no finito"));
    let tail_len = (((1.0 - confidence) * pnl_samples.len() as f64).ceil() as usize).max(1).min(pnl_samples.len());
    let var_idx = tail_len - 1;
    let var = -pnl_samples[var_idx];
    let es = -(pnl_samples[..tail_len].iter().sum::<f64>() / tail_len as f64);

    Ok(PnlDistribution { mean: estimate.mean, std_error: estimate.std_error, var, es, n_paths: estimate.n_paths })
}

/// Sensibilidad ("Greek") pathwise bajo P de `spec_json` respecto de uno de los tres parametros
/// de `GbmP` (`"spot"`/`"mu"`/`"volatility"`) -- extension de `api::payoff_sensitivity_gbm_q` a P
/// (PLAN_GREEKS.md §5.1/§11 Fase 7: "se EXTIENDE a GbmP -- mismo mecanismo, mismos 3 parametros
/// s0/mu/sigma, para PayoffForecastP"). A diferencia de la version Q, `GbmPModel::capabilities()`
/// NUNCA declara `supports_early_exercise_regression` (ver `cpp/engine/src/model.cpp`), asi que un
/// contrato con `ContractOp::Exercise` ya deberia haber sido rechazado en el preflight de
/// capacidades del lado C++ antes de llegar aqui -- el chequeo de `contains_exercise` de abajo es
/// defensa en profundidad (mismo principio que `api::payoff_sensitivity_gbm_q`: nunca asumir que
/// el preflight de otra capa es la unica linea de defensa), no un fallback a bump-and-reval como
/// en Q (no hay ningun mecanismo LSM bajo P que perturbar). Sin descuento (ver el doc-comment del
/// modulo): la derivada es la del PROPIO cashflow no descontado, igual que `forecast_gbm_p`.
pub fn payoff_sensitivity_gbm_p(
    spec_json: &str,
    observable: &str,
    greek: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    if contains_exercise(&payoff) {
        return Err(
            "payoff: sensibilidad pathwise bajo P no soportada para contratos con Exercise (GbmP no declara \
             supports_early_exercise_regression, no existe fallback bump-and-reval bajo P para este caso)"
                .to_string(),
        );
    }
    let greek_kind = GbmPGreek::parse(greek)?;
    let (times, columns) = simulate_gbm_p_columns(&payoff, s0, mu, sigma, n_paths, seed)?;
    let n_paths_usize = n_paths as usize;

    let mut samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let f64_path = SinglePath { times: &times, values: &values, sigma };
        let states = resolve_trigger_states(&payoff, &f64_path, bridge_seed_for_path(seed, path_idx));
        let dual_path = GbmDualPath::<Dual>::new_p(&times, &values, s0, mu, sigma, greek_kind);
        let ledger = evaluate_dual(&payoff, &dual_path, &f64_path, &states);
        samples.push(ledger.iter().map(|cf| cf.amount.deriv).sum());
    }

    Ok(mc::aggregate(&samples, mc::Z_95))
}

/// Extension bajo P de `api::payoff_sensitivity2_gbm_q` (Gamma via likelihood ratio,
/// PLAN_HYPERDUAL.md §5): mismo mecanismo que `payoff_sensitivity_gbm_p` extiende a
/// `payoff_sensitivity_gbm_q` -- sustituye `(r,q)` por `(mu,0.0)` en la formula lognormal (mismo
/// truco que `GbmDualPath::new_p`) y no descuenta. Solo soportado para contratos de una unica
/// fecha terminal y `greek == "spot"`.
pub fn payoff_sensitivity2_gbm_p(
    spec_json: &str,
    observable: &str,
    greek: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let t = lrm::single_terminal_time(&payoff)?;
    if GbmPGreek::parse(greek)? != GbmPGreek::Spot {
        return Err(format!(
            "payoff: Gamma via likelihood ratio bajo P solo soportada para 'spot' (recibido '{greek}')"
        ));
    }
    let (_times, columns) = simulate_gbm_p_columns(&payoff, s0, mu, sigma, n_paths, seed)?;
    let n_paths_usize = n_paths as usize;

    let mut samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let s_t = columns[0][path_idx];
        let present_value = undiscounted_ledger_sum_at_terminal(&payoff, t, s_t, sigma, seed, path_idx);
        let z = lrm::recover_terminal_z(s0, mu, 0.0, sigma, t, s_t);
        samples.push(present_value * lrm::gamma_weight(z, s0, sigma, t));
    }

    Ok(mc::aggregate(&samples, mc::Z_95))
}

/// Extension bajo P de `api::payoff_sensitivity_cross_gbm_q` (Vanna via likelihood ratio): mismo
/// alcance/criterio que `payoff_sensitivity2_gbm_p`, solo el par `("spot","volatility")`.
pub fn payoff_sensitivity_cross_gbm_p(
    spec_json: &str,
    observable: &str,
    risk_factor: &str,
    cross_factor: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let t = lrm::single_terminal_time(&payoff)?;
    let is_vanna_pair = matches!(
        (GbmPGreek::parse(risk_factor)?, GbmPGreek::parse(cross_factor)?),
        (GbmPGreek::Spot, GbmPGreek::Volatility) | (GbmPGreek::Volatility, GbmPGreek::Spot)
    );
    if !is_vanna_pair {
        return Err(format!(
            "payoff: derivada cruzada via likelihood ratio bajo P solo soportada para el par ('spot', \
             'volatility') (Vanna) -- recibido ('{risk_factor}', '{cross_factor}')"
        ));
    }
    let (_times, columns) = simulate_gbm_p_columns(&payoff, s0, mu, sigma, n_paths, seed)?;
    let n_paths_usize = n_paths as usize;

    let mut samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let s_t = columns[0][path_idx];
        let present_value = undiscounted_ledger_sum_at_terminal(&payoff, t, s_t, sigma, seed, path_idx);
        let z = lrm::recover_terminal_z(s0, mu, 0.0, sigma, t, s_t);
        samples.push(present_value * lrm::vanna_weight(z, s0, sigma, t));
    }

    Ok(mc::aggregate(&samples, mc::Z_95))
}

/// Extension bajo P de `api::payoff_local_hessian_gbm_q` (PLAN_BACKWARD.md §9 Fase 1): mismo
/// mecanismo que `payoff_local_hessian_gbm_q` extiende a `payoff_sensitivity2_gbm_q`/
/// `payoff_sensitivity_cross_gbm_q` -- sustituye `(r,q)` por `(mu,0.0)` en la formula lognormal
/// (mismo truco que `payoff_sensitivity2_gbm_p`) y no descuenta. UNA sola llamada a
/// `simulate_gbm_p_columns`, reutilizada para las tres entradas del Hessiano local. Solo
/// contratos de una unica fecha terminal.
pub fn payoff_local_hessian_gbm_p(
    spec_json: &str,
    observable: &str,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<LocalHessianEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let t = lrm::single_terminal_time(&payoff)?;
    let (_times, columns) = simulate_gbm_p_columns(&payoff, s0, mu, sigma, n_paths, seed)?;
    let n_paths_usize = n_paths as usize;

    let mut gamma_samples = Vec::with_capacity(n_paths_usize);
    let mut volga_samples = Vec::with_capacity(n_paths_usize);
    let mut vanna_samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let s_t = columns[0][path_idx];
        let present_value = undiscounted_ledger_sum_at_terminal(&payoff, t, s_t, sigma, seed, path_idx);
        let z = lrm::recover_terminal_z(s0, mu, 0.0, sigma, t, s_t);
        let weights = lrm::local_hessian_weights(z, s0, sigma, t);
        gamma_samples.push(present_value * weights.gamma);
        volga_samples.push(present_value * weights.volga);
        vanna_samples.push(present_value * weights.vanna);
    }

    Ok(LocalHessianEstimate {
        gamma: mc::aggregate(&gamma_samples, mc::Z_95),
        volga: mc::aggregate(&volga_samples, mc::Z_95),
        vanna: mc::aggregate(&vanna_samples, mc::Z_95),
    })
}

fn undiscounted_ledger_sum_at_terminal(payoff: &CompiledPayoff, t: f64, s_t: f64, sigma: f64, seed: u64, path_idx: usize) -> f64 {
    let times = [t];
    let values = [s_t];
    let f64_path = SinglePath { times: &times, values: &values, sigma };
    let (ledger, _events) = evaluate_with_events_seeded(payoff, &f64_path, bridge_seed_for_path(seed, path_idx));
    ledger.iter().map(|cf| cf.amount).sum()
}

/// `true` si `spec_json` compila y depende del subyacente en una unica fecha terminal -- extension
/// bajo P de `api::payoff_supports_second_order_lrm` (mismo criterio, mismo `payoff::lrm`).
pub fn payoff_supports_second_order_lrm_p(spec_json: &str) -> Result<bool, String> {
    let payoff = compile(spec_json)?;
    Ok(lrm::single_terminal_time(&payoff).is_ok())
}

#[cfg(test)]
mod tests {
    use super::*;

    // Grupo POSITION_EXIT (§4.3): TP al +20%, SL al -10%, metrica ReturnFromEntry, liquidacion
    // AtHit `quantity*(EventValue(...) - entry_price)`. Construido a mano en JSON (mismo nivel de
    // abstraccion que `api.rs::tests::up_and_in_call_json`) en vez de con el builder C++
    // `templates::first_of_take_profit_stop_loss` (Fase 2) porque este archivo prueba solo el
    // lado Rust -- el AST via C++ ya se prueba en `cpp/engine/tests/payoff/test_tp_sl.cpp` y se
    // probara de nuevo end-to-end bajo P en `test_gbm_measures_p.cpp` (Fase 7).
    fn tp_sl_json(entry_price: f64, quantity: f64, take_profit: f64, stop_loss: f64, monitoring_times: &[f64]) -> String {
        let times_json = monitoring_times.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(",");
        format!(
            r#"{{
                "schema": "engine.payoff/v1", "id": "TP_SL",
                "contract": {{
                    "type": "trigger", "id": "TAKE_PROFIT",
                    "monitoring_times": [{times_json}],
                    "condition": {{"type": "greater_equal",
                        "left": {{"type": "div",
                            "left": {{"type": "current", "observable": "EQ.SPOT.XYZ"}},
                            "right": {{"type": "constant", "value": {entry_price}}}}},
                        "right": {{"type": "constant", "value": {tp_ratio}}}}},
                    "monitoring": "discrete", "settlement": "at_hit", "priority": 10, "latch": true,
                    "on_hit": {{"type": "cashflow", "currency": "USD",
                        "amount": {{"type": "mul",
                            "left": {{"type": "constant", "value": {quantity}}},
                            "right": {{"type": "sub",
                                "left": {{"type": "event_value", "event": "TAKE_PROFIT", "observable": "EQ.SPOT.XYZ"}},
                                "right": {{"type": "constant", "value": {entry_price}}}}}}}}},
                    "on_miss": {{"type": "trigger", "id": "STOP_LOSS",
                        "monitoring_times": [{times_json}],
                        "condition": {{"type": "less_equal",
                            "left": {{"type": "div",
                                "left": {{"type": "current", "observable": "EQ.SPOT.XYZ"}},
                                "right": {{"type": "constant", "value": {entry_price}}}}},
                            "right": {{"type": "constant", "value": {sl_ratio}}}}},
                        "monitoring": "discrete", "settlement": "at_hit", "priority": 20, "latch": true,
                        "on_hit": {{"type": "cashflow", "currency": "USD",
                            "amount": {{"type": "mul",
                                "left": {{"type": "constant", "value": {quantity}}},
                                "right": {{"type": "sub",
                                    "left": {{"type": "event_value", "event": "STOP_LOSS", "observable": "EQ.SPOT.XYZ"}},
                                    "right": {{"type": "constant", "value": {entry_price}}}}}}}}},
                        "on_miss": {{"type": "zero"}}
                    }}
                }}
            }}"#,
            tp_ratio = 1.0 + take_profit,
            sl_ratio = 1.0 - stop_loss,
        )
    }

    #[test]
    fn forecast_preflight_rejects_an_observable_the_model_does_not_generate() {
        let spec = tp_sl_json(100.0, 1.0, 0.20, 0.10, &[0.25, 0.5, 0.75, 1.0]);
        let err = forecast_gbm_p(&spec, "EQ.SPOT.OTHER_TICKER", 100.0, 0.05, 0.2, 1_000, 7)
            .expect_err("un observable distinto del generado por el modelo debe fallar en preflight");
        assert!(err.contains("EQ.SPOT.XYZ"));
        assert!(err.contains("no generado"));
    }

    #[test]
    fn a_higher_physical_drift_produces_a_higher_forecast_take_profit_hit_probability() {
        // Criterio de aceptacion de Fase 7: "muestra diferencias esperadas de hit/P&L en un
        // fixture con drift P distinto de r-q". Mismo TP/SL, dos drifts fisicos distintos: uno
        // apenas mayor que cero, otro claramente alcista -- con el mismo `sigma`, un drift mas
        // alcista debe alcanzar el TP (+20%) con mas frecuencia.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let monitoring_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();
        let spec = tp_sl_json(100.0, 1.0, 0.20, 0.10, &monitoring_times);
        let (s0, sigma) = (100.0, 0.2);
        let (n_paths, seed) = (200_000, 7);

        let low_drift =
            hit_probability_gbm_p(&spec, "TAKE_PROFIT", "EQ.SPOT.XYZ", s0, 0.02, sigma, n_paths, seed).unwrap();
        let high_drift =
            hit_probability_gbm_p(&spec, "TAKE_PROFIT", "EQ.SPOT.XYZ", s0, 0.40, sigma, n_paths, seed).unwrap();

        let tolerance = 8.0 * (low_drift.std_error + high_drift.std_error);
        assert!(
            high_drift.mean > low_drift.mean + tolerance,
            "low_drift={} (se={}) high_drift={} (se={}) deberia ser claramente mayor",
            low_drift.mean,
            low_drift.std_error,
            high_drift.mean,
            high_drift.std_error
        );
    }

    #[test]
    fn forecast_pnl_is_higher_under_a_more_bullish_physical_drift() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let monitoring_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();
        let spec = tp_sl_json(100.0, 1.0, 0.20, 0.10, &monitoring_times);
        let (s0, sigma) = (100.0, 0.2);
        let (n_paths, seed) = (200_000, 11);

        let low_drift = forecast_gbm_p(&spec, "EQ.SPOT.XYZ", s0, 0.02, sigma, n_paths, seed).unwrap();
        let high_drift = forecast_gbm_p(&spec, "EQ.SPOT.XYZ", s0, 0.40, sigma, n_paths, seed).unwrap();

        let tolerance = 8.0 * (low_drift.std_error + high_drift.std_error);
        assert!(
            high_drift.mean > low_drift.mean + tolerance,
            "low_drift={} (se={}) high_drift={} (se={}) deberia ser claramente mayor",
            low_drift.mean,
            low_drift.std_error,
            high_drift.mean,
            high_drift.std_error
        );
    }

    #[test]
    fn pnl_distribution_expected_shortfall_is_at_least_as_severe_as_value_at_risk() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let monitoring_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();
        let spec = tp_sl_json(100.0, 1.0, 0.20, 0.10, &monitoring_times);

        let dist =
            pnl_distribution_gbm_p(&spec, "EQ.SPOT.XYZ", 100.0, 0.05, 0.2, 100_000, 13, 0.95).unwrap();

        assert!(dist.es >= dist.var, "es={} deberia ser >= var={}", dist.es, dist.var);
    }

    #[test]
    fn pnl_distribution_rejects_a_confidence_outside_zero_one() {
        let spec = tp_sl_json(100.0, 1.0, 0.20, 0.10, &[0.25, 0.5, 0.75, 1.0]);
        assert!(pnl_distribution_gbm_p(&spec, "EQ.SPOT.XYZ", 100.0, 0.05, 0.2, 1_000, 7, 1.0).is_err());
        assert!(pnl_distribution_gbm_p(&spec, "EQ.SPOT.XYZ", 100.0, 0.05, 0.2, 1_000, 7, -0.1).is_err());
    }

    const CALL_JSON: &str = r#"{
        "schema": "engine.payoff/v1", "id": "CALL",
        "contract": {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD",
            "amount": {"type": "max",
                "left": {"type": "sub",
                    "left": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": 1.0},
                    "right": {"type": "constant", "value": 100.0}},
                "right": {"type": "constant", "value": 0.0}}}}
    }"#;

    #[test]
    fn sensitivity_spot_delta_under_p_matches_finite_difference_of_forecast() {
        // Criterio de aceptacion de Fase 7 (PLAN_GREEKS.md §11): pathwise bajo P coincide con
        // bump-and-reval dentro de tolerancia -- aqui el "bump-and-reval" de referencia es una
        // diferencia central de `forecast_gbm_p` (no descontada), MISMO seed en ambas evaluaciones
        // (numeros aleatorios comunes, PLAN_GREEKS.md §4.4).
        let (s0, mu, sigma) = (100.0, 0.05, 0.2);
        let (n_paths, seed) = (200_000, 7);
        let h = 0.5;

        let pathwise = payoff_sensitivity_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", "spot", s0, mu, sigma, n_paths, seed).unwrap();
        let up = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 + h, mu, sigma, n_paths, seed).unwrap();
        let down = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 - h, mu, sigma, n_paths, seed).unwrap();
        let finite_difference = (up.mean - down.mean) / (2.0 * h);

        let tolerance = 8.0 * pathwise.std_error;
        assert!(
            (pathwise.mean - finite_difference).abs() < tolerance,
            "pathwise={} (se={}) finite_difference={} deberian coincidir dentro de tolerancia",
            pathwise.mean,
            pathwise.std_error,
            finite_difference
        );
    }

    // PLAN_HYPERDUAL.md §5: Gamma/Vanna via likelihood ratio bajo P, mismo seed en las evaluaciones
    // de forecast_gbm_p (numeros aleatorios comunes) -- mismo criterio que
    // sensitivity_spot_delta_under_p_matches_finite_difference_of_forecast.
    #[test]
    fn sensitivity2_gamma_under_p_matches_second_finite_difference_of_forecast() {
        let (s0, mu, sigma) = (100.0, 0.05, 0.2);
        let (n_paths, seed) = (500_000, 7);
        let h = 1.0;

        let gamma = payoff_sensitivity2_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", "spot", s0, mu, sigma, n_paths, seed).unwrap();
        let up = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 + h, mu, sigma, n_paths, seed).unwrap();
        let base = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0, mu, sigma, n_paths, seed).unwrap();
        let down = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 - h, mu, sigma, n_paths, seed).unwrap();
        let finite_difference = (up.mean - 2.0 * base.mean + down.mean) / (h * h);

        let tolerance = 8.0 * gamma.std_error;
        assert!(
            (gamma.mean - finite_difference).abs() < tolerance,
            "gamma={} (se={}) finite_difference={} deberian coincidir dentro de tolerancia",
            gamma.mean,
            gamma.std_error,
            finite_difference
        );
    }

    #[test]
    fn sensitivity_cross_vanna_under_p_matches_mixed_finite_difference_of_forecast() {
        let (s0, mu, sigma) = (100.0, 0.05, 0.2);
        let (n_paths, seed) = (500_000, 11);
        let (h_spot, h_vol) = (1.0, 0.002);

        let vanna = payoff_sensitivity_cross_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", "spot", "volatility", s0, mu, sigma, n_paths, seed)
            .unwrap();
        let up_up = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 + h_spot, mu, sigma + h_vol, n_paths, seed).unwrap();
        let up_down = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 + h_spot, mu, sigma - h_vol, n_paths, seed).unwrap();
        let down_up = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 - h_spot, mu, sigma + h_vol, n_paths, seed).unwrap();
        let down_down = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0 - h_spot, mu, sigma - h_vol, n_paths, seed).unwrap();
        let finite_difference =
            (up_up.mean - up_down.mean - down_up.mean + down_down.mean) / (4.0 * h_spot * h_vol);

        let tolerance = 8.0 * vanna.std_error;
        assert!(
            (vanna.mean - finite_difference).abs() < tolerance,
            "vanna={} (se={}) finite_difference={} deberian coincidir dentro de tolerancia",
            vanna.mean,
            vanna.std_error,
            finite_difference
        );
    }

    // PLAN_BACKWARD.md §9 Fase 1, extension bajo P: mismo criterio que el test hermano en
    // `api.rs` -- Gamma/Vanna deben coincidir bit a bit con las llamadas separadas (mismo
    // seed/n_paths/formula) y Volga debe ser consistente con la segunda diferencia finita de
    // `forecast_gbm_p` respecto de sigma.
    #[test]
    fn local_hessian_under_p_matches_separate_calls_bit_for_bit_and_volga_matches_finite_difference() {
        let (s0, mu, sigma) = (100.0, 0.05, 0.2);
        let (n_paths, seed) = (500_000, 7);

        let hessian = payoff_local_hessian_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0, mu, sigma, n_paths, seed).unwrap();
        let gamma_separate = payoff_sensitivity2_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", "spot", s0, mu, sigma, n_paths, seed).unwrap();
        let vanna_separate = payoff_sensitivity_cross_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", "spot", "volatility", s0, mu, sigma, n_paths, seed)
            .unwrap();

        assert_eq!(hessian.gamma, gamma_separate, "misma semilla/n_paths/formula -> identico bit a bit");
        assert_eq!(hessian.vanna, vanna_separate, "misma semilla/n_paths/formula -> identico bit a bit");

        let h = 1e-3;
        let up = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0, mu, sigma + h, n_paths, seed).unwrap();
        let base = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0, mu, sigma, n_paths, seed).unwrap();
        let down = forecast_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", s0, mu, sigma - h, n_paths, seed).unwrap();
        let finite_difference = (up.mean - 2.0 * base.mean + down.mean) / (h * h);

        let tolerance = 8.0 * hessian.volga.std_error;
        assert!(
            (hessian.volga.mean - finite_difference).abs() < tolerance,
            "volga={} (se={}) finite_difference={} deberian coincidir dentro de tolerancia",
            hessian.volga.mean,
            hessian.volga.std_error,
            finite_difference
        );
    }

    #[test]
    fn sensitivity_rejects_an_unknown_p_greek() {
        let err = payoff_sensitivity_gbm_p(CALL_JSON, "EQ.SPOT.XYZ", "rate", 100.0, 0.05, 0.2, 1_000, 7)
            .expect_err("'rate' no es un parametro de GbmP");
        assert!(err.contains("rate"));
    }
}
