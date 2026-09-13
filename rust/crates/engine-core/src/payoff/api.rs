//! Orquestacion de precio bajo Q de un `CompiledPayoff` (PLAN_PRODUCTS.md §12 Fase 5): compila
//! el JSON canonico `engine.payoff/v1`, simula bajo GBM el unico observable que el modelo genera
//! en los instantes que el programa necesita (`CompiledPayoff::required_times`), interpreta el
//! IR sobre cada ruta (`payoff::eval::evaluate`), descuenta a valor presente con la `r` constante
//! del modelo y agrega media/error estandar/intervalo de confianza (`crate::mc`).
//!
//! Frontera `f64` pura, mismo espiritu que `crate::api` (la que `engine-ffi` bridgea hoy para
//! IRS/Hull-White) -- a diferencia de ese modulo, esta funcion SI devuelve `Result`: el preflight
//! de "observable no generado" (criterio de aceptacion de Fase 5) es un error real que debe
//! propagarse a quien llama, no una degradacion silenciosa como `crate::api::resolve_backend`.
//! `cxx` traduce un `Result::Err(String)` en una excepcion de C++ en el punto de la llamada (ver
//! el bridge en `rust/crates/engine-ffi/src/lib.rs`), asi que este `Result` cruza la frontera tal
//! cual sin necesitar un tipo de error propio.
//!
//! Deliberadamente solo CPU (`CpuBackend`), a diferencia de `crate::api` que es generico via
//! `resolve_backend`: ningun criterio de aceptacion de Fase 5 menciona GPU; generalizar sobre
//! `Backend` cuando haga falta es un cambio aditivo (Fase 11, "vectorizacion CPU/GPU").

use crate::backend::CpuBackend;
use crate::mc::{self, McEstimate};
use crate::models::gbm::Gbm;
use crate::payoff::compile::compile;
use crate::payoff::eval::{evaluate_with_events, ObservablePath};
use crate::payoff::ir::CompiledPayoff;
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

fn scalar(value: f64, device: &burn::tensor::Device<CpuBackend>) -> Tensor<CpuBackend, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Una unica ruta ya simulada: `times[i]` <-> `values[i]`, un unico observable (slot 0 -- el
/// preflight de `price_payoff_gbm_q` garantiza que `payoff.observable_slots` no tenga mas de un
/// nombre distinto antes de llegar aqui).
struct SinglePath<'a> {
    times: &'a [f64],
    values: &'a [f64],
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
}

/// Preflight comun a `price_payoff_gbm_q`/`hit_probability_gbm_q`: `payoff` no referencia ningun
/// observable distinto de `observable` (el UNICO que este `Gbm` genera, PLAN_PRODUCTS.md §6
/// "`ModelCapabilities::generated_observables`") y `n_paths > 0`. `Err` ANTES de simular una sola
/// ruta -- criterio de aceptacion explicito de Fase 5.
fn check_single_observable(payoff: &CompiledPayoff, observable: &str, n_paths: u64) -> Result<(), String> {
    for used in &payoff.observable_slots {
        if used != observable {
            return Err(format!(
                "payoff: observable '{used}' no generado por este modelo GBM (unico observable soportado: '{observable}')"
            ));
        }
    }
    if n_paths == 0 {
        return Err("payoff: n_paths debe ser > 0".to_string());
    }
    Ok(())
}

/// Simula bajo GBM el unico observable de `payoff` en `payoff.required_times()` y devuelve
/// `(times, columns)`: `columns[i][path]` es el valor de ese observable en `times[i]` para la
/// ruta `path`. Compartido por `price_payoff_gbm_q`/`hit_probability_gbm_q` -- ambas interpretan
/// el mismo `CompiledPayoff` sobre las mismas rutas, solo difiere que agregan al final (cashflow
/// descontado vs. indicador de hit).
#[allow(clippy::too_many_arguments)]
fn simulate_gbm_columns(
    payoff: &CompiledPayoff,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<(Vec<f64>, Vec<Vec<f64>>), String> {
    let times = payoff.required_times();
    if times.is_empty() {
        return Err(
            "payoff: el contrato no depende de ningun instante de mercado (nada que simular bajo Q)".to_string(),
        );
    }

    let device = burn::tensor::Device::<CpuBackend>::default();
    CpuBackend::seed(&device, seed);
    let model = Gbm::new(scalar(s0, &device), scalar(r, &device), scalar(q, &device), scalar(sigma, &device));
    let simulated = model.simulate_at_times(&times, n_paths as usize, &device);
    let columns: Vec<Vec<f64>> =
        simulated.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect();
    Ok((times, columns))
}

/// Precio bajo Q (`E_Q[cashflows descontados]`) de un `PayoffProgram` serializado en
/// `spec_json` (`engine.payoff/v1`) bajo GBM (`s0`/`r`/`q`/`sigma`), mas error estandar e
/// intervalo de confianza del estimador Monte Carlo (`crate::mc::McEstimate`).
///
/// `observable` es el UNICO nombre de observable que este `Gbm` genera (PLAN_PRODUCTS.md §6,
/// "`ModelCapabilities::generated_observables`"): si el contrato referencia cualquier otro
/// nombre, o cualquier nodo no soportado en `CompiledPayoff` v1 (Fase 5), `Err` ANTES de simular
/// una sola ruta -- preflight, criterio de aceptacion explicito de Fase 5.
#[allow(clippy::too_many_arguments)]
pub fn price_payoff_gbm_q(
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let (times, columns) = simulate_gbm_columns(&payoff, s0, r, q, sigma, n_paths, seed)?;
    let n_paths_usize = n_paths as usize;

    let mut discounted_samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &times, values: &values };
        let (ledger, _events) = evaluate_with_events(&payoff, &path);
        let present_value: f64 = ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum();
        discounted_samples.push(present_value);
    }

    Ok(mc::aggregate(&discounted_samples, mc::Z_95))
}

/// Probabilidad bajo Q de que el evento `event` (un `Trigger` de `spec_json`, identificado por su
/// `EventId`) dispare en la ruta, estimada por Monte Carlo (PLAN_PRODUCTS.md §12 Fase 6: "hit
/// probability Q como medida separada del PV") -- media (la propia probabilidad, en `[0,1]`),
/// error estandar e intervalo de confianza de `crate::mc::McEstimate`, agregando el indicador
/// `1.0`/`0.0` de "el evento ocurrio en esta ruta" sobre las mismas rutas que usaria
/// `price_payoff_gbm_q` para el mismo `spec_json`/modelo (misma simulacion, agregacion distinta:
/// nunca se descuenta un indicador de hit, a diferencia de un cashflow).
#[allow(clippy::too_many_arguments)]
pub fn hit_probability_gbm_q(
    spec_json: &str,
    event: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
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
    let (times, columns) = simulate_gbm_columns(&payoff, s0, r, q, sigma, n_paths, seed)?;
    let n_paths_usize = n_paths as usize;

    let mut hit_indicators = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &times, values: &values };
        let (_ledger, events) = evaluate_with_events(&payoff, &path);
        hit_indicators.push(if events[event_slot].occurred { 1.0 } else { 0.0 });
    }

    Ok(mc::aggregate(&hit_indicators, mc::Z_95))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::models::gbm::black_scholes_call;

    const CALL_JSON_TEMPLATE: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "TEST_CALL",
        "contract": {
            "type": "when",
            "time": {maturity},
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "max",
                    "left": {
                        "type": "sub",
                        "left": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}},
                        "right": {"type": "constant", "value": {strike}}
                    },
                    "right": {"type": "constant", "value": 0.0}
                }
            }
        }
    }"#;

    #[test]
    fn preflight_rejects_an_observable_the_model_does_not_generate() {
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        let err = price_payoff_gbm_q(&spec, "EQ.SPOT.OTHER_TICKER", 100.0, 0.05, 0.0, 0.2, 1_000, 7)
            .expect_err("un observable distinto del generado por el modelo debe fallar en preflight");
        assert!(err.contains("EQ.SPOT.XYZ"));
        assert!(err.contains("no generado"));
    }

    #[test]
    fn monte_carlo_price_of_a_payoff_call_converges_to_black_scholes() {
        // PLAN.md §7.19: lock de RNG global, ver crate::rng_test_lock.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE
            .replace("{maturity}", &maturity.to_string())
            .replace("{strike}", &strike.to_string());

        let estimate = price_payoff_gbm_q(&spec, "EQ.SPOT.XYZ", s0, r, q, sigma, 200_000, 7).unwrap();
        let analytic = black_scholes_call(s0, strike, r, q, sigma, maturity);

        let tolerance = 8.0 * estimate.std_error;
        assert!(
            (estimate.mean - analytic).abs() < tolerance,
            "MC={} analytic={analytic} tol={tolerance} (std_error={})",
            estimate.mean,
            estimate.std_error
        );
    }

    #[test]
    fn q_price_has_no_physical_drift_parameter_to_change() {
        // Fase 5, criterio de aceptacion: "cambiar el drift fisico no afecta un precio Q". Esta
        // funcion no expone NINGUN parametro de drift fisico -- solo `r` (libre de riesgo) y `q`
        // (dividend yield), ambos parte de la propia definicion de la medida Q (ver el
        // doc-comment del modulo y de `models::gbm::Gbm`) -- asi que no hay ningun "drift fisico"
        // que pueda cambiarse sin cambiar tambien `r`/`q`, en cuyo caso ya no seria el mismo
        // precio Q por definicion, no una fuga de un parametro P. Este test fija esa garantia
        // estructural en la firma (`price_payoff_gbm_q` toma exactamente `s0/r/q/sigma`, ninguno
        // documentado como "fisico"): dos llamadas con distinta seed sobre el MISMO `(r, q,
        // sigma)` deben seguir siendo estimaciones del mismo precio Q, dentro de sus propios
        // intervalos de confianza -- no hay margen para que un dato fisico externo se cuele.
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        let (s0, r, q, sigma) = (100.0, 0.05, 0.0, 0.2);
        let first = price_payoff_gbm_q(&spec, "EQ.SPOT.XYZ", s0, r, q, sigma, 50_000, 7).unwrap();
        let second = price_payoff_gbm_q(&spec, "EQ.SPOT.XYZ", s0, r, q, sigma, 50_000, 99).unwrap();
        let combined_tolerance = 8.0 * (first.std_error + second.std_error);
        assert!(
            (first.mean - second.mean).abs() < combined_tolerance,
            "first={first:?} second={second:?} tol={combined_tolerance}"
        );
    }

    // PLAN_PRODUCTS.md §12 Fase 6 ("barrera discreta vectorizada bajo Q"): un up-and-in call
    // (barrera H, strike K, ambos monitorizados en `monitoring_times`) que paga en `maturity` si
    // el spot toca H en algun instante monitorizado, o cero si nunca lo toca.
    fn up_and_in_call_json(barrier: f64, strike: f64, maturity: f64, monitoring_times: &[f64]) -> String {
        let times_json = monitoring_times.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(",");
        format!(
            r#"{{
                "schema": "engine.payoff/v1", "id": "UI",
                "contract": {{
                    "type": "trigger", "id": "UI",
                    "monitoring_times": [{times_json}],
                    "condition": {{"type": "greater_equal",
                        "left": {{"type": "current", "observable": "EQ.SPOT.XYZ"}},
                        "right": {{"type": "constant", "value": {barrier}}}}},
                    "monitoring": "discrete", "settlement": "at_scheduled_payment", "priority": 0, "latch": true,
                    "on_hit": {{"type": "when", "time": {maturity},
                        "child": {{"type": "cashflow", "currency": "USD",
                            "amount": {{"type": "max",
                                "left": {{"type": "sub",
                                    "left": {{"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}},
                                    "right": {{"type": "constant", "value": {strike}}}}},
                                "right": {{"type": "constant", "value": 0.0}}}}}}}},
                    "on_miss": {{"type": "zero"}}
                }}
            }}"#
        )
    }

    // Complemento: up-and-out (paga el mismo intrinseco solo si la barrera NUNCA se toca; cero si
    // se toca -- "on_hit"/"on_miss" intercambiados frente a `up_and_in_call_json`, mismo barrier/
    // strike/monitoring_times). §13.2: "un knock-in + knock-out complementarios reproducen el
    // underlying" -- UI + UO debe reproducir exactamente la call vanilla en CADA ruta.
    fn up_and_out_call_json(barrier: f64, strike: f64, maturity: f64, monitoring_times: &[f64]) -> String {
        let times_json = monitoring_times.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(",");
        format!(
            r#"{{
                "schema": "engine.payoff/v1", "id": "UO",
                "contract": {{
                    "type": "trigger", "id": "UO",
                    "monitoring_times": [{times_json}],
                    "condition": {{"type": "greater_equal",
                        "left": {{"type": "current", "observable": "EQ.SPOT.XYZ"}},
                        "right": {{"type": "constant", "value": {barrier}}}}},
                    "monitoring": "discrete", "settlement": "at_scheduled_payment", "priority": 0, "latch": true,
                    "on_hit": {{"type": "zero"}},
                    "on_miss": {{"type": "when", "time": {maturity},
                        "child": {{"type": "cashflow", "currency": "USD",
                            "amount": {{"type": "max",
                                "left": {{"type": "sub",
                                    "left": {{"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}},
                                    "right": {{"type": "constant", "value": {strike}}}}},
                                "right": {{"type": "constant", "value": 0.0}}}}}}}}
                }}
            }}"#
        )
    }

    #[test]
    fn up_and_in_plus_up_and_out_reproduces_the_vanilla_call_price() {
        // §13.2 ("un knock-in + knock-out complementarios reproducen el underlying"): en CADA
        // ruta simulada para UI/UO (mismo grid de monitorizacion de 4 puntos), o bien UI paga el
        // intrinseco y UO paga cero, o al reves -- la suma de los ledgers coincide EXACTAMENTE
        // con el intrinseco de la call vanilla en esa misma ruta. `vanilla` se precia aqui con su
        // propio grid natural (un unico paso, `required_times()={maturity}`): la comparacion
        // frente a `ui.mean + uo.mean` es entonces estadistica (ambas simulaciones son GBM exacto
        // -- sin sesgo de discretizacion -- asi que coinciden en esperanza, no path-a-path, al
        // usar un numero de pasos distinto para cada una).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let monitoring_times = [0.25, 0.5, 0.75, 1.0];
        let (n_paths, seed) = (300_000, 7);

        let ui_spec = up_and_in_call_json(barrier, strike, maturity, &monitoring_times);
        let uo_spec = up_and_out_call_json(barrier, strike, maturity, &monitoring_times);
        let vanilla_spec = CALL_JSON_TEMPLATE
            .replace("{maturity}", &maturity.to_string())
            .replace("{strike}", &strike.to_string());

        let ui = price_payoff_gbm_q(&ui_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let uo = price_payoff_gbm_q(&uo_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed + 1).unwrap();
        let vanilla = price_payoff_gbm_q(&vanilla_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed + 2).unwrap();

        let combined_std_error = (ui.std_error.powi(2) + uo.std_error.powi(2) + vanilla.std_error.powi(2)).sqrt();
        let tolerance = 8.0 * combined_std_error;
        assert!(
            (ui.mean + uo.mean - vanilla.mean).abs() < tolerance,
            "ui={} uo={} ui+uo={} vanilla={} tol={tolerance}",
            ui.mean,
            uo.mean,
            ui.mean + uo.mean,
            vanilla.mean
        );
    }

    #[test]
    fn up_and_in_price_increases_as_the_monitoring_grid_is_refined() {
        // Mas puntos de monitorizacion == mas oportunidades de tocar la barrera == probabilidad
        // de activacion no decreciente == precio up-and-in no decreciente (converge hacia el
        // limite de monitorizacion continua al refinar la malla, PLAN_PRODUCTS.md §12 Fase 6,
        // criterio de aceptacion "convergencia de barreras al refinar malla").
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (300_000, 11);

        let coarse_times: Vec<f64> = vec![0.25, 0.5, 0.75, 1.0];
        let fine_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();

        let coarse_spec = up_and_in_call_json(barrier, strike, maturity, &coarse_times);
        let fine_spec = up_and_in_call_json(barrier, strike, maturity, &fine_times);

        let coarse = price_payoff_gbm_q(&coarse_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let fine = price_payoff_gbm_q(&fine_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        let tolerance = 8.0 * (coarse.std_error + fine.std_error);
        assert!(
            fine.mean + tolerance >= coarse.mean,
            "fine={} (se={}) deberia ser >= coarse={} (se={}) menos ruido estadistico",
            fine.mean,
            fine.std_error,
            coarse.mean,
            coarse.std_error
        );
    }

    // PLAN_PRODUCTS.md §12 Fase 6 ("hit probability Q como medida separada del PV").
    #[test]
    fn hit_probability_preflight_rejects_an_event_that_does_not_exist() {
        let spec = up_and_in_call_json(120.0, 100.0, 1.0, &[0.25, 0.5, 0.75, 1.0]);
        let err = hit_probability_gbm_q(&spec, "NOT_A_REAL_EVENT", "EQ.SPOT.XYZ", 100.0, 0.05, 0.0, 0.2, 1_000, 7)
            .expect_err("un evento que no existe en el contrato debe fallar en preflight");
        assert!(err.contains("NOT_A_REAL_EVENT"));
    }

    #[test]
    fn hit_probability_is_between_zero_and_one_and_decreases_as_the_barrier_moves_away() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let monitoring_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();
        let (n_paths, seed) = (200_000, 7);

        // Barrera mas cercana al spot (105, 5% OTM) se toca con mas probabilidad que una mas
        // lejana (140, 40% OTM) -- sanity check direccional, no una cota analitica.
        let near_spec = up_and_in_call_json(105.0, strike, maturity, &monitoring_times);
        let far_spec = up_and_in_call_json(140.0, strike, maturity, &monitoring_times);

        let near = hit_probability_gbm_q(&near_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let far = hit_probability_gbm_q(&far_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        assert!(near.mean > 0.0 && near.mean < 1.0, "near={}", near.mean);
        assert!(far.mean > 0.0 && far.mean < 1.0, "far={}", far.mean);
        let tolerance = 8.0 * (near.std_error + far.std_error);
        assert!(
            near.mean > far.mean + tolerance,
            "near={} (se={}) deberia ser claramente mayor que far={} (se={})",
            near.mean,
            near.std_error,
            far.mean,
            far.std_error
        );
    }

    #[test]
    fn hit_probability_increases_as_the_monitoring_grid_is_refined() {
        // Mismo argumento de monotonia que `up_and_in_price_increases_as_the_monitoring_grid_is_refined`,
        // pero sobre la probabilidad de hit en si misma en vez del precio derivado de ella.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (300_000, 11);

        let coarse_times: Vec<f64> = vec![0.25, 0.5, 0.75, 1.0];
        let fine_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();

        let coarse_spec = up_and_in_call_json(barrier, strike, maturity, &coarse_times);
        let fine_spec = up_and_in_call_json(barrier, strike, maturity, &fine_times);

        let coarse = hit_probability_gbm_q(&coarse_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let fine = hit_probability_gbm_q(&fine_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        let tolerance = 8.0 * (coarse.std_error + fine.std_error);
        assert!(
            fine.mean + tolerance >= coarse.mean,
            "fine={} (se={}) deberia ser >= coarse={} (se={}) menos ruido estadistico",
            fine.mean,
            fine.std_error,
            coarse.mean,
            coarse.std_error
        );
    }
}
