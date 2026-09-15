//! Sensibilidades ("Greeks") pathwise de un `CompiledPayoff` bajo GBM (PLAN_PRODUCTS.md §12 Fase
//! 11, item pendiente "AAD con fallback a bump-and-reval para sensibilidades del pricer Monte
//! Carlo GBM de payoff").
//!
//! **Metodo (pathwise / Broadie-Glasserman, no `Autodiff<CpuBackend>` de Burn)**: bajo GBM,
//! `S_t = s0 * exp((r - q - 0.5*sigma^2)*t + sigma*W_t)`. Dada una ruta YA SIMULADA en `f64`
//! (`S_t` conocido en cada `t` de `payoff.required_times()`), se recupera el Browniano REALIZADO
//! `W_t = (ln(S_t/s0) - (r-q-0.5*sigma^2)*t) / sigma` y se lo trata como una CONSTANTE -- la ruta
//! aleatoria queda fija, exactamente igual que en un bump-and-reval con numeros aleatorios
//! comunes. Sobre esa `W_t` fija, `S_t` se recalcula con aritmetica de `super::dual::Dual`
//! parametrizando el UNO de `(s0, r, q, sigma)` que se este derivando: la derivada resultante es
//! la derivada pathwise EXACTA de `S_t` respecto de ese parametro (sin ruido de discretizacion de
//! un bump finito), que luego se propaga por el arbol de `ScalarOp` con la regla de la cadena de
//! operador sobrecargado. Es el mismo resultado que produciria un AAD reverse-mode sobre el mismo
//! calculo, solo que implementado hacia adelante porque aqui basta una unica direccion por
//! llamada.
//!
//! **Que decide fijo, no dual (documentado, no oculto -- PLAN_PRODUCTS.md §16)**: la ramificacion
//! discreta del contrato (`ContractOp::If`, `Trigger`, la decision de `Exercise`) se resuelve UNA
//! VEZ sobre la ruta `f64` realizada (`eval::resolve_trigger_states`, o Longstaff-Schwartz para
//! `Exercise`) y se mantiene FIJA para la pasada dual -- es la justificacion estandar del metodo
//! pathwise: el conjunto de rutas donde perturbar infinitesimalmente el parametro cambiaria esa
//! decision tiene medida cero, salvo patologias. Para un contrato con `ContractOp::Exercise`, sin
//! embargo, re-decidir bajo el parametro perturbado SI cambia de forma no trivial la politica de
//! Longstaff-Schwartz completa (la regresion se ajusta sobre TODO el lote, no ruta a ruta) --
//! ahi el metodo pathwise de este modulo ya no aplica limpiamente, y `payoff::api::
//! payoff_sensitivity_gbm_q` usa en su lugar el fallback bump-and-reval que PLAN_PRODUCTS.md
//! preveia explicitamente (numeros aleatorios comunes: mismo `seed` en ambas valoraciones
//! bumped, para que solo cambie el parametro perturbado).

use super::dual::Dual;
use super::eval::{eval_predicate, EventStateResolved, ObservablePath};
use super::ir::{CompiledPayoff, ContractOp, ScalarOp, SettlementMode};

/// Un cashflow sin agregar cuyo importe es un `Dual` en vez de un `f64` -- mismo papel que
/// `eval::PathCashflow`, para la pasada de sensibilidad.
pub(crate) struct PathCashflowDual {
    pub(crate) payment_time: f64,
    pub(crate) amount: Dual,
}

/// Equivalente dual de `eval::ObservablePath`: valores de UN observable de una ruta ya simulada,
/// como `Dual` respecto del parametro elegido. Separado de `ObservablePath` (en vez de generico
/// sobre ambos) para no tocar el interprete `f64` existente, ya probado -- ver el doc-comment del
/// modulo.
pub(crate) trait DualObservablePath {
    fn value_at(&self, slot: usize, time: f64) -> Dual;
}

/// Los cuatro parametros de `models::gbm::Gbm` respecto de los que se puede pedir una
/// sensibilidad. Nombres de cadena estables (`GbmGreek::parse`) porque cruzan la misma frontera
/// de `&str` que `backend` en `payoff::api` (consistencia con el resto del modulo, PLAN_PRODUCTS.md
/// §7.4: "no cambia layout por comodidad").
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum GbmGreek {
    Spot,
    Rate,
    DividendYield,
    Volatility,
}

impl GbmGreek {
    pub(crate) fn parse(name: &str) -> Result<Self, String> {
        match name {
            "spot" => Ok(GbmGreek::Spot),
            "rate" => Ok(GbmGreek::Rate),
            "dividend_yield" => Ok(GbmGreek::DividendYield),
            "volatility" => Ok(GbmGreek::Volatility),
            other => Err(format!(
                "payoff: greek '{other}' desconocido (valores soportados: 'spot', 'rate', 'dividend_yield', 'volatility')"
            )),
        }
    }
}

/// `true` si `payoff` contiene al menos un `ContractOp::Exercise` -- puerta de entrada de
/// `payoff::api::payoff_sensitivity_gbm_q` para decidir entre la pasada pathwise de este modulo y
/// el fallback bump-and-reval (ver el doc-comment del modulo).
pub(crate) fn contains_exercise(payoff: &CompiledPayoff) -> bool {
    payoff.contract_ops.iter().any(|op| matches!(op, ContractOp::Exercise { .. }))
}

/// Recupera `S_t(param)` como `Dual` a partir de una ruta GBM `f64` ya simulada, fijando el
/// Browniano realizado (ver el doc-comment del modulo). `times`/`values` son paralelos, mismos
/// arrays que ya usa `payoff::api::SinglePath` para esa ruta.
pub(crate) struct GbmDualPath<'a> {
    times: &'a [f64],
    /// Browniano recuperado por indice de `times` (`w[i]` corresponde a `times[i]`).
    w: Vec<f64>,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    greek: GbmGreek,
}

impl<'a> GbmDualPath<'a> {
    pub(crate) fn new(times: &'a [f64], values: &[f64], s0: f64, r: f64, q: f64, sigma: f64, greek: GbmGreek) -> Self {
        assert_eq!(times.len(), values.len(), "payoff: times/values de longitud distinta");
        let drift_no_diffusion = r - q - 0.5 * sigma * sigma;
        let w = times
            .iter()
            .zip(values.iter())
            .map(|(&t, &s)| ((s / s0).ln() - drift_no_diffusion * t) / sigma)
            .collect();
        Self { times, w, s0, r, q, sigma, greek }
    }

    fn param_duals(&self) -> (Dual, Dual, Dual, Dual) {
        let s0_d = if self.greek == GbmGreek::Spot { Dual::variable(self.s0) } else { Dual::constant(self.s0) };
        let r_d = if self.greek == GbmGreek::Rate { Dual::variable(self.r) } else { Dual::constant(self.r) };
        let q_d = if self.greek == GbmGreek::DividendYield {
            Dual::variable(self.q)
        } else {
            Dual::constant(self.q)
        };
        let sigma_d = if self.greek == GbmGreek::Volatility {
            Dual::variable(self.sigma)
        } else {
            Dual::constant(self.sigma)
        };
        (s0_d, r_d, q_d, sigma_d)
    }

    /// El `Dual` de `r` usado internamente, expuesto para que quien descuenta el ledger
    /// (`payoff::api`) use exactamente el mismo `r` dual al construir el factor de descuento --
    /// si `greek == Rate`, descontar con un `r` constante ignoraria la sensibilidad del propio
    /// descuento (`rho` incluye tanto el termino de la ruta como el de `exp(-r*t)`).
    pub(crate) fn rate_dual(&self) -> Dual {
        self.param_duals().1
    }
}

impl DualObservablePath for GbmDualPath<'_> {
    fn value_at(&self, _slot: usize, time: f64) -> Dual {
        let idx = self
            .times
            .iter()
            .position(|&t| (t - time).abs() < 1e-9)
            .unwrap_or_else(|| panic!("payoff: tiempo {time} no simulado por el modelo (times={:?})", self.times));
        let (s0_d, r_d, q_d, sigma_d) = self.param_duals();
        let t = Dual::constant(self.times[idx]);
        let w = Dual::constant(self.w[idx]);
        let half = Dual::constant(0.5);
        let drift = (r_d - q_d - sigma_d * sigma_d * half) * t;
        let diffusion = sigma_d * w;
        s0_d * (drift + diffusion).exp()
    }
}

/// Evalua `payoff.scalar_ops[idx]` como `Dual` -- espejo exacto de `eval::eval_scalar`, mismo
/// conjunto de nodos, mismos mensajes de error; ver el doc-comment del modulo para el porque de
/// esta duplicacion deliberadamente pequena en vez de generalizar `eval::eval_scalar` sobre un
/// tipo numerico.
pub(crate) fn eval_scalar_dual(
    payoff: &CompiledPayoff,
    idx: usize,
    cursor: Option<f64>,
    path: &dyn DualObservablePath,
    states: &[EventStateResolved],
) -> Dual {
    match &payoff.scalar_ops[idx] {
        ScalarOp::Constant(v) => Dual::constant(*v),
        ScalarOp::Fixing { observable, time } => path.value_at(*observable, *time),
        ScalarOp::Current { observable } => {
            let t = cursor.expect("payoff: 'current' sin cursor de tiempo activo (falta un 'when' envolvente)");
            path.value_at(*observable, t)
        }
        ScalarOp::Add(l, r) => {
            eval_scalar_dual(payoff, *l, cursor, path, states) + eval_scalar_dual(payoff, *r, cursor, path, states)
        }
        ScalarOp::Sub(l, r) => {
            eval_scalar_dual(payoff, *l, cursor, path, states) - eval_scalar_dual(payoff, *r, cursor, path, states)
        }
        ScalarOp::Mul(l, r) => {
            eval_scalar_dual(payoff, *l, cursor, path, states) * eval_scalar_dual(payoff, *r, cursor, path, states)
        }
        ScalarOp::Div(l, r) => {
            let denominator = eval_scalar_dual(payoff, *r, cursor, path, states);
            assert!(denominator.value != 0.0, "payoff: division por cero");
            eval_scalar_dual(payoff, *l, cursor, path, states) / denominator
        }
        ScalarOp::Neg(x) => -eval_scalar_dual(payoff, *x, cursor, path, states),
        ScalarOp::Abs(x) => eval_scalar_dual(payoff, *x, cursor, path, states).abs(),
        ScalarOp::Exp(x) => eval_scalar_dual(payoff, *x, cursor, path, states).exp(),
        ScalarOp::Log(x) => eval_scalar_dual(payoff, *x, cursor, path, states).ln(),
        ScalarOp::Pow(base, exponent) => eval_scalar_dual(payoff, *base, cursor, path, states)
            .powf(eval_scalar_dual(payoff, *exponent, cursor, path, states)),
        ScalarOp::Min(l, r) => {
            eval_scalar_dual(payoff, *l, cursor, path, states).min(eval_scalar_dual(payoff, *r, cursor, path, states))
        }
        ScalarOp::Max(l, r) => {
            eval_scalar_dual(payoff, *l, cursor, path, states).max(eval_scalar_dual(payoff, *r, cursor, path, states))
        }
        ScalarOp::Clamp { value, low, high } => {
            let v = eval_scalar_dual(payoff, *value, cursor, path, states);
            let lo = eval_scalar_dual(payoff, *low, cursor, path, states);
            let hi = eval_scalar_dual(payoff, *high, cursor, path, states);
            v.max(lo).min(hi)
        }
        ScalarOp::EventValue { event, observable } => {
            let state = &states[*event];
            let event_name = &payoff.event_slots[*event];
            if !state.occurred {
                panic!("payoff: EventValue: el evento '{event_name}' no ha ocurrido en esta ruta");
            }
            let t = state
                .first_hit_time
                .expect("payoff: EventValue: evento 'occurred' sin first_hit_time (estado inconsistente)");
            // Recalcula el valor capturado como Dual en vez de leer `captured_values` (que solo
            // guarda el `f64` realizado, ver `eval::EventStateResolved`): mismo valor real,
            // ahora con su sensibilidad, porque `DualObservablePath::value_at` es una funcion
            // pura de `(observable, time)` -- ninguna captura adicional que mantener aparte.
            path.value_at(*observable, t)
        }
    }
}

/// Evalua `payoff.contract_ops[idx]` acumulando cashflows duales en `out` -- espejo exacto de
/// `eval::eval_contract`, salvo `ContractOp::If`, cuyo predicado se evalua sobre `f64_path`/
/// `states` (la ramificacion se decide una vez y se mantiene fija, ver el doc-comment del modulo).
#[allow(clippy::too_many_arguments)]
pub(crate) fn eval_contract_dual(
    payoff: &CompiledPayoff,
    idx: usize,
    cursor: Option<f64>,
    dual_path: &dyn DualObservablePath,
    f64_path: &dyn ObservablePath,
    states: &[EventStateResolved],
    out: &mut Vec<PathCashflowDual>,
) {
    match &payoff.contract_ops[idx] {
        ContractOp::Zero => {}
        ContractOp::Cashflow { amount } => {
            let t = cursor.expect("payoff: 'cashflow' sin cursor de tiempo activo (falta un 'when' envolvente)");
            out.push(PathCashflowDual {
                payment_time: t,
                amount: eval_scalar_dual(payoff, *amount, cursor, dual_path, states),
            });
        }
        ContractOp::Give(child) => {
            let start = out.len();
            eval_contract_dual(payoff, *child, cursor, dual_path, f64_path, states, out);
            for cf in &mut out[start..] {
                cf.amount = -cf.amount;
            }
        }
        ContractOp::Both(children) => {
            for &child in children {
                eval_contract_dual(payoff, child, cursor, dual_path, f64_path, states, out);
            }
        }
        ContractOp::Scale { factor, child } => {
            let f = eval_scalar_dual(payoff, *factor, cursor, dual_path, states);
            let start = out.len();
            eval_contract_dual(payoff, *child, cursor, dual_path, f64_path, states, out);
            for cf in &mut out[start..] {
                cf.amount = cf.amount * f;
            }
        }
        ContractOp::If { condition, if_true, if_false } => {
            if eval_predicate(payoff, *condition, cursor, f64_path, states) {
                eval_contract_dual(payoff, *if_true, cursor, dual_path, f64_path, states, out);
            } else {
                eval_contract_dual(payoff, *if_false, cursor, dual_path, f64_path, states, out);
            }
        }
        ContractOp::When { time, child } => {
            eval_contract_dual(payoff, *child, Some(*time), dual_path, f64_path, states, out)
        }
        ContractOp::Trigger { event, settlement, on_hit, on_miss, .. } => {
            let state = &states[*event];
            if state.occurred {
                let new_cursor = match settlement {
                    SettlementMode::AtHit => state.first_hit_time,
                    SettlementMode::AtScheduledPayment => cursor,
                };
                eval_contract_dual(payoff, *on_hit, new_cursor, dual_path, f64_path, states, out);
            } else {
                eval_contract_dual(payoff, *on_miss, cursor, dual_path, f64_path, states, out);
            }
        }
        ContractOp::Exercise { event, exercise_value, continuation, .. } => {
            let state = &states[*event];
            if state.occurred {
                let t = state
                    .first_hit_time
                    .expect("payoff: Exercise 'occurred' sin first_hit_time (estado de decision inconsistente)");
                out.push(PathCashflowDual {
                    payment_time: t,
                    amount: eval_scalar_dual(payoff, *exercise_value, Some(t), dual_path, states),
                });
            } else {
                eval_contract_dual(payoff, *continuation, cursor, dual_path, f64_path, states, out);
            }
        }
    }
}

/// Interpreta `payoff` sobre una unica ruta y devuelve el ledger pathwise dual sin descontar --
/// version dual de `eval::evaluate_with_resolved_states`. `states` debe venir de
/// `eval::resolve_trigger_states` sobre la MISMA ruta `f64` que subyace a `dual_path` (mismos
/// tiempos/valores, ver `GbmDualPath::new`).
pub(crate) fn evaluate_dual(
    payoff: &CompiledPayoff,
    dual_path: &dyn DualObservablePath,
    f64_path: &dyn ObservablePath,
    states: &[EventStateResolved],
) -> Vec<PathCashflowDual> {
    let mut out = Vec::new();
    eval_contract_dual(payoff, payoff.root, None, dual_path, f64_path, states, &mut out);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::payoff::compile::compile;
    use crate::payoff::eval::resolve_trigger_states;

    struct FixedPath<'a> {
        times: &'a [f64],
        values: &'a [f64],
    }
    impl ObservablePath for FixedPath<'_> {
        fn value_at(&self, _slot: usize, time: f64) -> f64 {
            let idx = self.times.iter().position(|&t| (t - time).abs() < 1e-9).unwrap();
            self.values[idx]
        }
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
    fn gbm_dual_path_recovers_s0_derivative_matching_finite_difference() {
        // S_T bajo un W_t fijo es log-lineal en s0 (dS_T/ds0 = S_T/s0 exactamente) -- confirma
        // que GbmDualPath::value_at reproduce esa identidad sin pasar por el interprete.
        let (s0, r, q, sigma, t) = (100.0, 0.05, 0.0, 0.2, 1.0);
        let times = [t];
        let s_t = 137.0; // una realizacion cualquiera > 0
        let values = [s_t];
        let path = GbmDualPath::new(&times, &values, s0, r, q, sigma, GbmGreek::Spot);
        let dual = path.value_at(0, t);
        assert!((dual.value - s_t).abs() < 1e-9);
        let expected_deriv = s_t / s0;
        assert!((dual.deriv - expected_deriv).abs() < 1e-9, "deriv={} expected={}", dual.deriv, expected_deriv);
    }

    #[test]
    fn pathwise_delta_of_a_call_matches_the_indicator_of_being_in_the_money() {
        // Derivada pathwise del payoff de una call (max(S_T-K,0)) respecto de s0, en una ruta
        // conocida: si S_T > K, d(max(S_T-K,0))/ds0 = dS_T/ds0 = S_T/s0 (la rama "S_T-K" esta
        // activa); si S_T < K, la derivada es 0 (rama "0" constante, sin dependencia de s0).
        let payoff = compile(CALL_JSON).unwrap();
        let (s0, r, q, sigma, t) = (100.0, 0.05, 0.0, 0.2, 1.0);
        let times = [t];

        for &s_t in &[137.0, 60.0] {
            let values = [s_t];
            let f64_path = FixedPath { times: &times, values: &values };
            let states = resolve_trigger_states(&payoff, &f64_path, 0);
            let dual_path = GbmDualPath::new(&times, &values, s0, r, q, sigma, GbmGreek::Spot);
            let ledger = evaluate_dual(&payoff, &dual_path, &f64_path, &states);
            assert_eq!(ledger.len(), 1);
            let expected_deriv = if s_t > 100.0 { s_t / s0 } else { 0.0 };
            assert!(
                (ledger[0].amount.deriv - expected_deriv).abs() < 1e-9,
                "s_t={s_t} deriv={} expected={expected_deriv}",
                ledger[0].amount.deriv
            );
        }
    }

    #[test]
    fn contains_exercise_detects_the_exercise_contract_op_and_only_that_one() {
        let call = compile(CALL_JSON).unwrap();
        assert!(!contains_exercise(&call));

        let exercise_json = r#"{
            "schema": "engine.payoff/v1", "id": "EX",
            "contract": {"type": "exercise", "id": "EX", "dates": [0.5],
                "exercise_value": {"type": "constant", "value": 1.0},
                "continuation": {"type": "when", "time": 1.0, "child": {"type": "cashflow",
                    "currency": "USD", "amount": {"type": "constant", "value": 1.0}}}}
        }"#;
        let exercise = compile(exercise_json).unwrap();
        assert!(contains_exercise(&exercise));
    }

    #[test]
    fn greek_parse_accepts_the_four_gbm_parameters_and_rejects_others() {
        assert_eq!(GbmGreek::parse("spot").unwrap(), GbmGreek::Spot);
        assert_eq!(GbmGreek::parse("rate").unwrap(), GbmGreek::Rate);
        assert_eq!(GbmGreek::parse("dividend_yield").unwrap(), GbmGreek::DividendYield);
        assert_eq!(GbmGreek::parse("volatility").unwrap(), GbmGreek::Volatility);
        assert!(GbmGreek::parse("theta").is_err());
    }
}
