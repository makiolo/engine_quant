//! Precio bajo Q de un `CompiledPayoff` multi-activo sobre `models::gbm_basket::GbmBasket`
//! (PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2 punto 2): generaliza `payoff::api::price_payoff_gbm_q` de
//! un unico observable a `n_assets` observables correlacionados.
//!
//! **Nada que tocar en el compilador**: `CompiledPayoff::observable_slots`
//! (`compile::Compiler::observable_slot`) ya resuelve CUALQUIER numero de nombres de observable
//! distintos a indices, sin cambios (ver el doc-comment de `payoff::ir::CompiledPayoff::
//! observable_slots`, escrito antes de que ningun modelo multi-activo existiera). Lo unico que
//! faltaba era la capa de SIMULACION/evaluacion: un `ObservablePath` que sepa indexar en el
//! activo correcto segun `slot` (`MultiAssetPath` abajo, a diferencia de `payoff::api::SinglePath`
//! que ignora `slot` porque solo hay un observable posible), mas la resolucion de
//! `observable_slots` (orden del CONTRATO) contra los observables que el MODELO genera (orden
//! del basket) POR NOMBRE, no por posicion (`resolve_slot_to_asset`).

use crate::backend::{resolve_backend, ComputeBackend, CpuBackend};
use crate::mc::{self, McEstimate};
use crate::models::gbm_basket::GbmBasket;
use crate::payoff::api::bridge_seed_for_path;
use crate::payoff::compile::compile;
use crate::payoff::eval::{evaluate_with_events_seeded, ObservablePath};
use crate::payoff::ir::CompiledPayoff;
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

fn scalar<B: Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Una unica ruta ya simulada de TODOS los activos del basket: `values[time_idx][asset_idx]` es
/// el valor de ese activo en `times[time_idx]`, para la ruta ya seleccionada por la llamante.
/// `slot_to_asset[slot]` resuelve el indice de `CompiledPayoff::observable_slots` (orden del
/// CONTRATO) al indice de activo del MODELO (orden de `GbmBasket`/`observables`) -- resuelto UNA
/// VEZ antes de simular ninguna ruta (`resolve_slot_to_asset`), por NOMBRE.
struct MultiAssetPath<'a> {
    times: &'a [f64],
    values: &'a [Vec<f64>],
    slot_to_asset: &'a [usize],
    sigmas: &'a [f64],
}

impl ObservablePath for MultiAssetPath<'_> {
    fn value_at(&self, slot: usize, time: f64) -> f64 {
        let asset = self.slot_to_asset[slot];
        let idx = self
            .times
            .iter()
            .position(|&t| (t - time).abs() < 1e-9)
            .unwrap_or_else(|| {
                panic!("payoff: tiempo {time} no simulado por el modelo (times={:?})", self.times)
            });
        self.values[idx][asset]
    }

    fn volatility(&self, slot: usize) -> f64 {
        self.sigmas[self.slot_to_asset[slot]]
    }
}

/// Resuelve `payoff.observable_slots` (orden del contrato) contra `observables` (orden del
/// modelo, `GbmBasket`) POR NOMBRE -- `Err` explicito ANTES de simular una sola ruta si el
/// contrato referencia un observable que el modelo no genera, mismo criterio de preflight que
/// `payoff::api::check_single_observable` generalizado de un unico nombre a una lista.
fn resolve_slot_to_asset(payoff: &CompiledPayoff, observables: &[String]) -> Result<Vec<usize>, String> {
    payoff
        .observable_slots
        .iter()
        .map(|slot_name| {
            observables.iter().position(|name| name == slot_name).ok_or_else(|| {
                format!(
                    "payoff: observable '{slot_name}' no generado por este GbmBasket (activos declarados: \
                     {observables:?})"
                )
            })
        })
        .collect()
}

/// El numerario de descuento de un `PayoffProgram` multi-activo bajo Q es, en el alcance minimo
/// de esta fase (PLAN_IMPROVE_NOTEBOOK.md Fase 3, criterio de aceptacion explicito: "un basket
/// call de 2 activos se precia"), la MISMA curva libre de riesgo domestica para todos los activos
/// -- un basket/spread/worst-of de varios activos en la misma moneda comparte numerario. Este
/// motor no modela hoy curvas de descuento por activo independientes de `r` (eso pertenece a un
/// modelo quanto/cross-currency mas completo, ver la nota de limitaciones documentada en
/// PLAN_IMPROVE_NOTEBOOK.md Fase 3). `Err` explicito si `r` no es la misma para todos los activos
/// (dentro de una tolerancia numerica) en vez de elegir una silenciosamente (`r[0]`) y fingir que
/// el resultado es correcto para un basket con `r` heterogenea -- "nunca aproximar en silencio"
/// (PLAN_PRODUCTS.md §16).
fn validate_common_discount_rate(r: &[f64]) -> Result<f64, String> {
    let r0 = r[0];
    if let Some(&other) = r.iter().find(|&&ri| (ri - r0).abs() > 1e-9) {
        return Err(format!(
            "payoff: GbmBasket requiere la MISMA tasa libre de riesgo 'r' para todos los activos en esta fase \
             (numerario de descuento unico) -- se recibieron valores distintos ({r0} vs {other}); un basket con \
             r heterogenea por activo (p.ej. cross-currency) no esta soportado, PLAN_IMPROVE_NOTEBOOK.md Fase 3"
        ));
    }
    Ok(r0)
}

/// Precio bajo Q (Monte Carlo, `GbmBasket`) de un `PayoffProgram` multi-activo
/// (PLAN_IMPROVE_NOTEBOOK.md Fase 3). Mismo contrato de error que `price_payoff_gbm_q`: `Err`
/// ANTES de simular si el contrato referencia un observable no generado, si `observables`/`s0`/
/// `r`/`q`/`sigma`/`correlation` no tienen formas consistentes (delegado en `GbmBasket::new`), o
/// si `r` no es homogenea entre activos (`validate_common_discount_rate`). `correlation_flat` es
/// la matriz de correlacion aplanada FILA A FILA (mismo convenio documentado en
/// `engine::GbmBasketModel::correlation()` en C++ y en `engine_typed.model.GbmBasket` en Python --
/// ninguna capa reordena).
///
/// **Fuera de alcance de esta fase** (documentado, no oculto -- PLAN_PRODUCTS.md §16): sin
/// `valuation_time`/Theta, sin sensibilidades pathwise/Gamma/Vanna, sin `Exercise`/Longstaff-
/// Schwartz, sin Brownian bridge continuo para barreras, sin curvas de descuento por activo -- ver
/// PLAN_IMPROVE_NOTEBOOK.md Fase 3, "limitaciones dejadas fuera a proposito".
#[allow(clippy::too_many_arguments)]
pub fn price_payoff_basket_gbm_q(
    backend: &str,
    spec_json: &str,
    observables: &[String],
    s0: &[f64],
    r: &[f64],
    q: &[f64],
    sigma: &[f64],
    correlation_flat: &[f64],
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            price_payoff_basket_gbm_q_on::<CpuBackend>(
                &device, spec_json, observables, s0, r, q, sigma, correlation_flat, n_paths, seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                price_payoff_basket_gbm_q_on::<crate::backend::GpuBackend>(
                    &device, spec_json, observables, s0, r, q, sigma, correlation_flat, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                price_payoff_basket_gbm_q_on::<CpuBackend>(
                    &device, spec_json, observables, s0, r, q, sigma, correlation_flat, n_paths, seed,
                )
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn price_payoff_basket_gbm_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    spec_json: &str,
    observables: &[String],
    s0: &[f64],
    r: &[f64],
    q: &[f64],
    sigma: &[f64],
    correlation_flat: &[f64],
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    if n_paths == 0 {
        return Err("payoff: n_paths debe ser > 0".to_string());
    }
    let n_assets = observables.len();
    if n_assets == 0 {
        return Err(
            "payoff: 'observables' no puede estar vacio (GbmBasket requiere al menos un activo)".to_string(),
        );
    }
    if s0.len() != n_assets || r.len() != n_assets || q.len() != n_assets || sigma.len() != n_assets {
        return Err(format!(
            "payoff: 'observables' ({n_assets} activos) y s0/r/q/sigma deben tener la misma longitud (s0={}, \
             r={}, q={}, sigma={})",
            s0.len(),
            r.len(),
            q.len(),
            sigma.len()
        ));
    }
    if correlation_flat.len() != n_assets * n_assets {
        return Err(format!(
            "payoff: 'correlation' aplanada debe tener {n_assets}x{n_assets}={} elementos (uno por activo \
             declarado), se recibieron {}",
            n_assets * n_assets,
            correlation_flat.len()
        ));
    }
    let discount_rate = validate_common_discount_rate(r)?;
    let slot_to_asset = resolve_slot_to_asset(&payoff, observables)?;

    let times = payoff.required_times();
    if times.is_empty() {
        return Err(
            "payoff: el contrato no depende de ningun instante de mercado (nada que simular bajo Q)".to_string(),
        );
    }

    B::seed(device, seed);
    let s0_t: Vec<Tensor<B, 1>> = s0.iter().map(|&v| scalar(v, device)).collect();
    let r_t: Vec<Tensor<B, 1>> = r.iter().map(|&v| scalar(v, device)).collect();
    let q_t: Vec<Tensor<B, 1>> = q.iter().map(|&v| scalar(v, device)).collect();
    let sigma_t: Vec<Tensor<B, 1>> = sigma.iter().map(|&v| scalar(v, device)).collect();
    let correlation: Vec<Vec<f64>> =
        (0..n_assets).map(|i| correlation_flat[i * n_assets..(i + 1) * n_assets].to_vec()).collect();
    let model = GbmBasket::<B>::new(s0_t, r_t, q_t, sigma_t, correlation)?;

    // simulated[time_idx][asset_idx] -> Tensor<B,1> forma [n_paths]
    let simulated = model.simulate_at_times(&times, n_paths as usize, device);
    // columns[time_idx][asset_idx][path] -> f64
    let columns: Vec<Vec<Vec<f64>>> = simulated
        .into_iter()
        .map(|per_asset| per_asset.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect())
        .collect();

    let n_paths_usize = n_paths as usize;
    let mut discounted_samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        // values[time_idx][asset_idx] para ESTA ruta.
        let values: Vec<Vec<f64>> = columns
            .iter()
            .map(|per_asset_at_t| per_asset_at_t.iter().map(|col| col[path_idx]).collect())
            .collect();
        let path = MultiAssetPath { times: &times, values: &values, slot_to_asset: &slot_to_asset, sigmas: sigma };
        let (ledger, _events) = evaluate_with_events_seeded(&payoff, &path, bridge_seed_for_path(seed, path_idx));
        let present_value: f64 =
            ledger.iter().map(|cf| cf.amount * (-discount_rate * cf.payment_time).exp()).sum();
        discounted_samples.push(present_value);
    }

    Ok(mc::aggregate(&discounted_samples, mc::Z_95))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn call_on_sum_json(strike: f64, maturity: f64, names: [&str; 2]) -> String {
        format!(
            r#"{{
                "schema": "engine.payoff/v1",
                "id": "BASKET_CALL",
                "contract": {{
                    "type": "when",
                    "time": {maturity},
                    "child": {{
                        "type": "cashflow",
                        "currency": "USD",
                        "amount": {{
                            "type": "max",
                            "left": {{
                                "type": "sub",
                                "left": {{
                                    "type": "add",
                                    "left": {{"type": "fixing", "observable": "{a}", "time": {maturity}}},
                                    "right": {{"type": "fixing", "observable": "{b}", "time": {maturity}}}
                                }},
                                "right": {{"type": "constant", "value": {strike}}}
                            }},
                            "right": {{"type": "constant", "value": 0.0}}
                        }}
                    }}
                }}
            }}"#,
            maturity = maturity,
            strike = strike,
            a = names[0],
            b = names[1],
        )
    }

    #[test]
    fn rejects_unknown_observable_before_simulating() {
        let spec = call_on_sum_json(100.0, 1.0, ["EQ.SPOT.A", "EQ.SPOT.C"]);
        let err = price_payoff_basket_gbm_q(
            "cpu",
            &spec,
            &["EQ.SPOT.A".to_string(), "EQ.SPOT.B".to_string()],
            &[100.0, 100.0],
            &[0.05, 0.05],
            &[0.0, 0.0],
            &[0.2, 0.2],
            &[1.0, 0.3, 0.3, 1.0],
            1_000,
            42,
        )
        .expect_err("'EQ.SPOT.C' no esta en la lista de observables del modelo, deberia rechazarse");
        assert!(err.contains("EQ.SPOT.C"), "err={err}");
    }

    #[test]
    fn rejects_heterogeneous_discount_rate() {
        let spec = call_on_sum_json(100.0, 1.0, ["EQ.SPOT.A", "EQ.SPOT.B"]);
        let err = price_payoff_basket_gbm_q(
            "cpu",
            &spec,
            &["EQ.SPOT.A".to_string(), "EQ.SPOT.B".to_string()],
            &[100.0, 100.0],
            &[0.05, 0.06],
            &[0.0, 0.0],
            &[0.2, 0.2],
            &[1.0, 0.3, 0.3, 1.0],
            1_000,
            42,
        )
        .expect_err("r heterogenea entre activos deberia rechazarse en esta fase");
        assert!(err.contains("MISMA tasa"), "err={err}");
    }

    #[test]
    fn basket_call_price_increases_with_correlation() {
        // Criterio de aceptacion explicito de PLAN_IMPROVE_NOTEBOOK.md Fase 3: el precio de una
        // basket CALL sobre la SUMA de activos con la misma vol sube cuando sube la correlacion
        // (mas correlacion => mas varianza de la suma => mas valor de la opcionalidad).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let spec = call_on_sum_json(200.0, 1.0, ["EQ.SPOT.A", "EQ.SPOT.B"]);
        let observables = vec!["EQ.SPOT.A".to_string(), "EQ.SPOT.B".to_string()];
        let s0 = [100.0, 100.0];
        let r = [0.03, 0.03];
        let q = [0.0, 0.0];
        let sigma = [0.25, 0.25];
        let n_paths = 100_000;
        let seed = 7;

        let low_corr = price_payoff_basket_gbm_q(
            "cpu", &spec, &observables, &s0, &r, &q, &sigma, &[1.0, 0.0, 0.0, 1.0], n_paths, seed,
        )
        .unwrap();
        let high_corr = price_payoff_basket_gbm_q(
            "cpu", &spec, &observables, &s0, &r, &q, &sigma, &[1.0, 0.9, 0.9, 1.0], n_paths, seed,
        )
        .unwrap();

        assert!(
            high_corr.mean > low_corr.mean,
            "high_corr={} deberia superar low_corr={} (mas correlacion => mas varianza de la suma => call mas cara)",
            high_corr.mean,
            low_corr.mean
        );
    }
}
