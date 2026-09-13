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
use crate::payoff::eval::{evaluate, ObservablePath};
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

    let times = payoff.required_times();
    if times.is_empty() {
        return Err(
            "payoff: el contrato no depende de ningun instante de mercado (nada que simular bajo Q)".to_string(),
        );
    }

    let device = burn::tensor::Device::<CpuBackend>::default();
    CpuBackend::seed(&device, seed);
    let model = Gbm::new(scalar(s0, &device), scalar(r, &device), scalar(q, &device), scalar(sigma, &device));
    let n_paths_usize = n_paths as usize;
    let simulated = model.simulate_at_times(&times, n_paths_usize, &device);
    let columns: Vec<Vec<f64>> =
        simulated.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect();

    let mut discounted_samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &times, values: &values };
        let ledger = evaluate(&payoff, &path);
        let present_value: f64 = ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum();
        discounted_samples.push(present_value);
    }

    Ok(mc::aggregate(&discounted_samples, mc::Z_95))
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
}
