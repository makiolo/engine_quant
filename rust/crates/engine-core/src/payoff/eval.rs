//! Interprete pathwise escalar de referencia sobre `CompiledPayoff` (PLAN_PRODUCTS.md §12 Fase 5:
//! "ejecucion pathwise escalar de referencia en Rust"; Fase 6: barreras/`Trigger` discretos).
//! Equivalente Rust, sobre el IR compilado, de `engine::payoff::ScenarioEvaluator::evaluate` en
//! C++ (que interpreta directamente el AST con punteros compartidos): recorre `payoff.root`
//! recursivamente, resolviendo cada nodo por indice en el `Vec` de su categoria en vez de doble
//! dispatch virtual.
//!
//! No descuenta ni decide medida probabilistica (igual que `ScenarioEvaluator` en C++, §5.2):
//! solo produce el ledger pathwise sin agregar para UNA ruta ya simulada. Quien llama
//! (`crate::models::gbm`/`crate::mc`) descuenta bajo Q y agrega sobre muchas rutas.
//!
//! **`Trigger` (Fase 6)**: se resuelve en dos pasadas, igual que `ScenarioEvaluator` en C++
//! (`resolve_trigger_states` en `scenario_evaluator.cpp`, ADR-P0-08): (1) `resolve_trigger_states`
//! recorre la union ordenada de `monitoring_times` de todos los `Trigger` del programa y resuelve
//! `EventStateResolved` por evento (primer hit, latch, prioridad/orden lexicografico de
//! `EventId` como desempate -- ADR-P0-03); (2) `eval_contract` interpreta el arbol normalmente,
//! consultando ese estado ya resuelto via `ContractOp::Trigger`/`ScalarOp::EventValue`/
//! `PredicateOp::EventOccurred`.
//!
//! **Brownian bridge (`Monitoring::ContinuousApproximation`, Fase 6, §4.2)**: entre dos instantes
//! de monitorizacion CONSECUTIVOS de un mismo `Trigger` (`t_prev`, `t`) donde el chequeo discreto
//! en `t` no detecto un hit, pero AMBOS extremos estan al mismo lado de la barrera, existe una
//! probabilidad analitica de que la ruta CONTINUA la haya cruzado dentro de `(t_prev, t)` sin que
//! la rejilla discreta lo capturara (formula estandar de bridge browniano/lognormal:
//! `exp(-2*ln(H/a)*ln(H/b) / (sigma^2*dt))`, ver `bridge_crossing_probability`). Para preservar la
//! semantica de `EventState` (occurred/first_hit_time booleanos, de los que dependen
//! `EventOccurred`/`EventValue`/TP-SL) esa probabilidad se materializa como un hit/no-hit
//! CRISPADO por ruta: se compara contra un sorteo uniforme independiente de un PRNG determinista
//! sembrado por ruta (`BridgeRng`, NUNCA el RNG de Burn que genera la propia trayectoria -- son
//! dos fuentes de aleatoriedad independientes por diseno). Limitacion documentada (no oculta,
//! PLAN_PRODUCTS.md §16): no hay correccion de bridge ANTES del primer `monitoring_times` de cada
//! `Trigger` (no hay un extremo previo con el que formar el intervalo); un usuario que necesite
//! esa cola debe incluir un punto de monitorizacion temprano.

use super::ir::{BarrierDirection, CompiledPayoff, ContractOp, MonitoringMode, PredicateOp, ScalarOp, SettlementMode};

/// Un cashflow sin agregar, en la moneda unica de `CompiledPayoff::currency`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PathCashflow {
    pub payment_time: f64,
    pub amount: f64,
}

/// Valores de observable de UNA ruta ya simulada. `slot` indexa
/// `CompiledPayoff::observable_slots`; `time` es siempre uno de los tiempos que el propio
/// simulador uso para generar esa ruta (el compilador ya resolvio los `Fixing`/`Current` del
/// contrato a esos mismos tiempos) -- una consulta con un tiempo que la ruta no cubre es un error
/// de programacion de quien llama, no un caso de negocio, de ahi el `panic` esperado en
/// implementaciones (a diferencia del C++ `EvaluationError`, que vive en el limite con datos de
/// mercado externos y por tanto SI es un `Result`).
pub trait ObservablePath {
    fn value_at(&self, slot: usize, time: f64) -> f64;

    /// Volatilidad (`sigma`) constante del observable `slot`, necesaria UNICAMENTE para la
    /// correccion de Brownian bridge de `Monitoring::ContinuousApproximation` (§4.2, Fase 6) --
    /// ver el doc-comment del modulo y `resolve_trigger_states`. Metodo por defecto que entra en
    /// panico: solo hace falta sobreescribirlo si el contrato contiene un `Trigger` con esa
    /// monitorizacion (invariante que ya garantiza `compile::compile` via `ContractOp::
    /// Trigger::bridge`); los implementadores de rutas deterministas/discretas existentes
    /// (fixtures de test, `SinglePath` cuando no hay bridge) no necesitan tocar nada.
    fn volatility(&self, _slot: usize) -> f64 {
        panic!(
            "payoff: ObservablePath::volatility no implementado (requerido por \
             Monitoring::ContinuousApproximation, Fase 6)"
        )
    }
}

/// PRNG determinista minimo (splitmix64) para los sorteos independientes de Brownian bridge --
/// deliberadamente SEPARADO del RNG de Burn que genera la propia trayectoria (§ doc-comment del
/// modulo): sembrado una vez por ruta (`resolve_trigger_states`), consume valores en orden
/// determinista (mismo orden de iteracion de tiempos/triggers en cada llamada), reproducible sin
/// depender del estado global de Burn ni de cuantos otros tensores se hayan generado antes.
struct BridgeRng {
    state: u64,
}

impl BridgeRng {
    fn new(seed: u64) -> Self {
        Self { state: seed }
    }

    fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniforme en `[0, 1)` con 53 bits de resolucion (mantisa de un `f64`).
    fn next_uniform(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 * (1.0 / (1u64 << 53) as f64)
    }
}

/// Probabilidad analitica (bridge browniano/lognormal) de que la ruta CONTINUA de un GBM haya
/// cruzado la barrera `barrier` en algun instante de `(t_prev, t)`, dado que los valores
/// observados en los extremos son `a`/`b` (ambos al mismo lado de `barrier`, ya comprobado por el
/// llamante) y la volatilidad es `sigma` (PLAN_PRODUCTS.md §4.2). Formula estandar (p.ej. Baldi
/// 1995 / Beaglehole): `exp(-2*ln(barrier/a)*ln(barrier/b) / (sigma^2*dt))`. `None` si los datos
/// no permiten aplicarla (extremo no positivo, `dt<=0` o `sigma<=0`).
fn bridge_crossing_probability(a: f64, b: f64, barrier: f64, sigma: f64, dt: f64) -> Option<f64> {
    if !(a > 0.0 && b > 0.0 && barrier > 0.0 && sigma > 0.0 && dt > 0.0) {
        return None;
    }
    let log_a = (barrier / a).ln();
    let log_b = (barrier / b).ln();
    Some((-2.0 * log_a * log_b / (sigma * sigma * dt)).exp())
}

/// El instante de `monitoring_times` (propios de UN `Trigger`, no la union global) mas cercano
/// pero estrictamente anterior a `t`, o `None` si `t` es el primero de esa lista -- el extremo
/// izquierdo del intervalo que la correccion de bridge necesita para ese `Trigger`.
fn previous_monitoring_time(monitoring_times: &[f64], t: f64) -> Option<f64> {
    monitoring_times.iter().copied().filter(|&mt| mt < t - 1e-9).fold(None, |acc, mt| match acc {
        Some(cur) if cur >= mt => Some(cur),
        _ => Some(mt),
    })
}

/// Resultado de resolver un evento sobre UNA ruta -- subconjunto publico de
/// `EventStateResolved` (que ademas guarda los valores capturados, uso interno de
/// `ScalarOp::EventValue`). Pensado para que una futura medida "HitProbability" (PLAN_PRODUCTS.md
/// §12 Fase 6) consuma `occurred`/`first_hit_time` por evento sin tocar los internos de
/// resolucion.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EventOutcome {
    pub occurred: bool,
    pub first_hit_time: Option<f64>,
}

/// Estado interno (por ruta) de un evento -- ver el doc-comment del modulo y
/// `event_state.hpp`/`EventState` en C++, que este tipo replica campo a campo.
#[derive(Debug, Clone)]
struct EventStateResolved {
    occurred: bool,
    first_hit_time: Option<f64>,
    /// Indexado por `observable_slot` (paralelo a `CompiledPayoff::observable_slots`); `None`
    /// hasta que el evento dispara y captura ese observable.
    captured_values: Vec<Option<f64>>,
}

impl EventStateResolved {
    fn new(n_observables: usize) -> Self {
        Self { occurred: false, first_hit_time: None, captured_values: vec![None; n_observables] }
    }
}

/// Evalua `payoff.scalar_ops[idx]`. `cursor` es el "instante activo" de ADR-P0-08
/// (PLAN_PRODUCTS.md): solo `ContractOp::When`/`ContractOp::Trigger` (settlement `AtHit`) lo
/// fijan; `Current` lo requiere. `states` es el resultado ya resuelto de
/// `resolve_trigger_states` -- `EventValue` lo consulta, nunca lo muta.
fn eval_scalar(
    payoff: &CompiledPayoff,
    idx: usize,
    cursor: Option<f64>,
    path: &dyn ObservablePath,
    states: &[EventStateResolved],
) -> f64 {
    match &payoff.scalar_ops[idx] {
        ScalarOp::Constant(v) => *v,
        ScalarOp::Fixing { observable, time } => path.value_at(*observable, *time),
        ScalarOp::Current { observable } => {
            let t = cursor.expect("payoff: 'current' sin cursor de tiempo activo (falta un 'when' envolvente)");
            path.value_at(*observable, t)
        }
        ScalarOp::Add(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states) + eval_scalar(payoff, *r, cursor, path, states)
        }
        ScalarOp::Sub(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states) - eval_scalar(payoff, *r, cursor, path, states)
        }
        ScalarOp::Mul(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states) * eval_scalar(payoff, *r, cursor, path, states)
        }
        ScalarOp::Div(l, r) => {
            let denominator = eval_scalar(payoff, *r, cursor, path, states);
            assert!(denominator != 0.0, "payoff: division por cero");
            eval_scalar(payoff, *l, cursor, path, states) / denominator
        }
        ScalarOp::Neg(x) => -eval_scalar(payoff, *x, cursor, path, states),
        ScalarOp::Abs(x) => eval_scalar(payoff, *x, cursor, path, states).abs(),
        ScalarOp::Exp(x) => eval_scalar(payoff, *x, cursor, path, states).exp(),
        ScalarOp::Log(x) => eval_scalar(payoff, *x, cursor, path, states).ln(),
        ScalarOp::Pow(base, exponent) => eval_scalar(payoff, *base, cursor, path, states)
            .powf(eval_scalar(payoff, *exponent, cursor, path, states)),
        ScalarOp::Min(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states).min(eval_scalar(payoff, *r, cursor, path, states))
        }
        ScalarOp::Max(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states).max(eval_scalar(payoff, *r, cursor, path, states))
        }
        ScalarOp::Clamp { value, low, high } => {
            let v = eval_scalar(payoff, *value, cursor, path, states);
            let lo = eval_scalar(payoff, *low, cursor, path, states);
            let hi = eval_scalar(payoff, *high, cursor, path, states);
            v.max(lo).min(hi)
        }
        ScalarOp::EventValue { event, observable } => {
            let state = &states[*event];
            let event_name = &payoff.event_slots[*event];
            if !state.occurred {
                panic!("payoff: EventValue: el evento '{event_name}' no ha ocurrido en esta ruta");
            }
            state.captured_values[*observable].unwrap_or_else(|| {
                let observable_name = &payoff.observable_slots[*observable];
                panic!(
                    "payoff: EventValue: '{observable_name}' no fue capturado por el evento '{event_name}'"
                )
            })
        }
    }
}

/// Evalua `payoff.predicate_ops[idx]`. `All`/`Any` cortocircuitan (ADR-P0-03), igual que exige el
/// AST de autoria en C++.
fn eval_predicate(
    payoff: &CompiledPayoff,
    idx: usize,
    cursor: Option<f64>,
    path: &dyn ObservablePath,
    states: &[EventStateResolved],
) -> bool {
    match &payoff.predicate_ops[idx] {
        PredicateOp::Greater(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states) > eval_scalar(payoff, *r, cursor, path, states)
        }
        PredicateOp::Less(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states) < eval_scalar(payoff, *r, cursor, path, states)
        }
        PredicateOp::GreaterEqual(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states) >= eval_scalar(payoff, *r, cursor, path, states)
        }
        PredicateOp::LessEqual(l, r) => {
            eval_scalar(payoff, *l, cursor, path, states) <= eval_scalar(payoff, *r, cursor, path, states)
        }
        PredicateOp::Eq { left, right, tolerance } => {
            (eval_scalar(payoff, *left, cursor, path, states) - eval_scalar(payoff, *right, cursor, path, states))
                .abs()
                <= *tolerance
        }
        PredicateOp::All(operands) => operands.iter().all(|&i| eval_predicate(payoff, i, cursor, path, states)),
        PredicateOp::Any(operands) => operands.iter().any(|&i| eval_predicate(payoff, i, cursor, path, states)),
        PredicateOp::Not(operand) => !eval_predicate(payoff, *operand, cursor, path, states),
        PredicateOp::Between { value, low, high, low_inclusive, high_inclusive } => {
            let v = eval_scalar(payoff, *value, cursor, path, states);
            let lo = eval_scalar(payoff, *low, cursor, path, states);
            let hi = eval_scalar(payoff, *high, cursor, path, states);
            let low_ok = if *low_inclusive { v >= lo } else { v > lo };
            let high_ok = if *high_inclusive { v <= hi } else { v < hi };
            low_ok && high_ok
        }
        PredicateOp::EventOccurred(event) => states[*event].occurred,
    }
}

/// Resuelve el `EventStateResolved` de todos los `Trigger` de `payoff` sobre `path` (§4.1,
/// ADR-P0-08): una unica pasada cronologica sobre la union ordenada de `monitoring_times`,
/// evaluando en cada fecha los triggers ahi programados por `priority` y, a igualdad, por orden
/// lexicografico de `event_slots[event]` (ADR-P0-03). La actualizacion de un evento es visible de
/// inmediato para el siguiente trigger de la misma fecha (bucle secuencial, no paralelo) --
/// permite implementar TP/SL por composicion (`Not(EventOccurred(la_otra_regla))`) sin campos
/// nuevos. `latch=true`: una vez ocurrido, el evento deja de reevaluarse. `latch=false`: el estado
/// refleja la condicion en la fecha de monitorizacion mas reciente.
///
/// `bridge_seed` siembra el `BridgeRng` de esta ruta (ver doc-comment del modulo): irrelevante si
/// `payoff` no contiene ningun `Trigger` con `Monitoring::ContinuousApproximation` (nunca se
/// instancia el generador en ese caso).
fn resolve_trigger_states(payoff: &CompiledPayoff, path: &dyn ObservablePath, bridge_seed: u64) -> Vec<EventStateResolved> {
    let mut bridge_rng = BridgeRng::new(bridge_seed);
    let n_observables = payoff.observable_slots.len();
    let mut states: Vec<EventStateResolved> =
        payoff.event_slots.iter().map(|_| EventStateResolved::new(n_observables)).collect();

    let has_triggers = payoff.contract_ops.iter().any(|op| matches!(op, ContractOp::Trigger { .. }));
    if !has_triggers {
        return states;
    }

    let mut all_times: Vec<f64> = Vec::new();
    for op in &payoff.contract_ops {
        if let ContractOp::Trigger { monitoring_times, .. } = op {
            for &t in monitoring_times {
                if !all_times.iter().any(|&existing: &f64| (existing - t).abs() < 1e-9) {
                    all_times.push(t);
                }
            }
        }
    }
    all_times.sort_by(|a, b| a.partial_cmp(b).expect("payoff: monitoring_time no finito"));

    for &t in &all_times {
        // (priority, event_slot, contract_op_idx) de cada Trigger programado en `t` que aun no
        // ha latcheado -- ordenado por (priority asc, EventId lexicografico asc), ADR-P0-03.
        let mut active: Vec<(i32, usize, usize)> = Vec::new();
        for (idx, op) in payoff.contract_ops.iter().enumerate() {
            if let ContractOp::Trigger { event, monitoring_times, latch, priority, .. } = op {
                let scheduled_here = monitoring_times.iter().any(|&mt| (mt - t).abs() < 1e-9);
                if !scheduled_here {
                    continue;
                }
                if *latch && states[*event].occurred {
                    continue;
                }
                active.push((*priority, *event, idx));
            }
        }
        active.sort_by(|a, b| {
            a.0.cmp(&b.0).then_with(|| payoff.event_slots[a.1].cmp(&payoff.event_slots[b.1]))
        });

        for (_, event, idx) in active {
            let (condition, latch, monitoring, bridge, monitoring_times) = match &payoff.contract_ops[idx] {
                ContractOp::Trigger { condition, latch, monitoring, bridge, monitoring_times, .. } => {
                    (*condition, *latch, *monitoring, *bridge, monitoring_times.clone())
                }
                _ => unreachable!("payoff: 'active' solo contiene indices de ContractOp::Trigger"),
            };
            let mut condition_true = eval_predicate(payoff, condition, Some(t), path, &states);

            // Correccion de Brownian bridge (§4.2, Fase 6, ver doc-comment del modulo): solo si
            // el chequeo discreto en `t` no detecto ya un hit, la monitorizacion es
            // ContinuousApproximation (compile::compile garantiza `bridge.is_some()` en ese caso)
            // y existe un extremo previo propio de ESTE Trigger con el que formar el intervalo.
            if !condition_true && monitoring == MonitoringMode::ContinuousApproximation {
                if let (Some(pattern), Some(t_prev)) =
                    (bridge, previous_monitoring_time(&monitoring_times, t))
                {
                    let a = path.value_at(pattern.observable, t_prev);
                    let b = path.value_at(pattern.observable, t);
                    let same_side_and_not_yet_hit = match pattern.direction {
                        BarrierDirection::Up => a < pattern.barrier && b < pattern.barrier,
                        BarrierDirection::Down => a > pattern.barrier && b > pattern.barrier,
                    };
                    if same_side_and_not_yet_hit {
                        let sigma = path.volatility(pattern.observable);
                        if let Some(p_cross) =
                            bridge_crossing_probability(a, b, pattern.barrier, sigma, t - t_prev)
                        {
                            if bridge_rng.next_uniform() < p_cross {
                                condition_true = true;
                            }
                        }
                    }
                }
            }

            if condition_true {
                states[event].occurred = true;
                states[event].first_hit_time = Some(t);
                for v in &mut states[event].captured_values {
                    *v = None;
                }
                // Captura TODO observable referenciado via EventValue{event, observable} en
                // cualquier parte del programa (no solo dentro de on_hit): igual que
                // DependencyVisitor::event_value_observables en C++, que se calcula sobre el
                // arbol completo antes de evaluar.
                for scalar_op in &payoff.scalar_ops {
                    if let ScalarOp::EventValue { event: e, observable } = scalar_op {
                        if *e == event {
                            states[event].captured_values[*observable] = Some(path.value_at(*observable, t));
                        }
                    }
                }
            } else if !latch {
                states[event].occurred = false;
                states[event].first_hit_time = None;
                for v in &mut states[event].captured_values {
                    *v = None;
                }
            }
        }
    }

    states
}

/// Evalua `payoff.contract_ops[idx]`, acumulando cashflows en `out`. `Give`/`Scale` aplican su
/// transformacion a TODOS los cashflows que su hijo produjo (ADR-P0-02), no solo al primero.
fn eval_contract(
    payoff: &CompiledPayoff,
    idx: usize,
    cursor: Option<f64>,
    path: &dyn ObservablePath,
    states: &[EventStateResolved],
    out: &mut Vec<PathCashflow>,
) {
    match &payoff.contract_ops[idx] {
        ContractOp::Zero => {}
        ContractOp::Cashflow { amount } => {
            let t = cursor.expect("payoff: 'cashflow' sin cursor de tiempo activo (falta un 'when' envolvente)");
            out.push(PathCashflow { payment_time: t, amount: eval_scalar(payoff, *amount, cursor, path, states) });
        }
        ContractOp::Give(child) => {
            let start = out.len();
            eval_contract(payoff, *child, cursor, path, states, out);
            for cf in &mut out[start..] {
                cf.amount = -cf.amount;
            }
        }
        ContractOp::Both(children) => {
            for &child in children {
                eval_contract(payoff, child, cursor, path, states, out);
            }
        }
        ContractOp::Scale { factor, child } => {
            let f = eval_scalar(payoff, *factor, cursor, path, states);
            let start = out.len();
            eval_contract(payoff, *child, cursor, path, states, out);
            for cf in &mut out[start..] {
                cf.amount *= f;
            }
        }
        ContractOp::If { condition, if_true, if_false } => {
            if eval_predicate(payoff, *condition, cursor, path, states) {
                eval_contract(payoff, *if_true, cursor, path, states, out);
            } else {
                eval_contract(payoff, *if_false, cursor, path, states, out);
            }
        }
        ContractOp::When { time, child } => eval_contract(payoff, *child, Some(*time), path, states, out),
        ContractOp::Trigger { event, settlement, on_hit, on_miss, .. } => {
            let state = &states[*event];
            if state.occurred {
                let new_cursor = match settlement {
                    SettlementMode::AtHit => state.first_hit_time,
                    SettlementMode::AtScheduledPayment => cursor,
                };
                eval_contract(payoff, *on_hit, new_cursor, path, states, out);
            } else {
                eval_contract(payoff, *on_miss, cursor, path, states, out);
            }
        }
    }
}

/// Interpreta `payoff` sobre una unica ruta y devuelve el ledger pathwise sin descontar, junto
/// al `EventOutcome` de cada evento (indexado como `payoff.event_slots`) -- equivalente Rust de
/// "Cashflows"/`ScenarioEvaluator::evaluate` (PLAN_PRODUCTS.md §12 Fase 1/4/6), pero sobre el IR
/// compilado. Usar esta version (en vez de `evaluate`) cuando ademas del ledger haga falta saber
/// si un evento concreto disparo (p.ej. una futura medida "HitProbability"). Equivalente a
/// `evaluate_with_events_seeded(payoff, path, 0)` -- sin ningun `Trigger` con `Monitoring::
/// ContinuousApproximation` el `bridge_seed` nunca se consume, asi que el valor fijo es
/// irrelevante para todo lo que ya soportaba esta funcion antes de Brownian bridge.
pub fn evaluate_with_events(
    payoff: &CompiledPayoff,
    path: &dyn ObservablePath,
) -> (Vec<PathCashflow>, Vec<EventOutcome>) {
    evaluate_with_events_seeded(payoff, path, 0)
}

/// Igual que `evaluate_with_events`, sembrando ademas el `BridgeRng` de esta ruta con
/// `bridge_seed` (PLAN_PRODUCTS.md §4.2, Fase 6) -- quien orquesta muchas rutas Monte Carlo
/// (`crate::payoff::api`) debe pasar un `bridge_seed` DISTINTO e independiente por ruta (nunca el
/// mismo `seed` que siembra el RNG de Burn que genera la propia trayectoria: son dos fuentes de
/// aleatoriedad independientes por diseno, ver el doc-comment del modulo).
pub fn evaluate_with_events_seeded(
    payoff: &CompiledPayoff,
    path: &dyn ObservablePath,
    bridge_seed: u64,
) -> (Vec<PathCashflow>, Vec<EventOutcome>) {
    let states = resolve_trigger_states(payoff, path, bridge_seed);
    let mut out = Vec::new();
    eval_contract(payoff, payoff.root, None, path, &states, &mut out);
    let outcomes =
        states.iter().map(|s| EventOutcome { occurred: s.occurred, first_hit_time: s.first_hit_time }).collect();
    (out, outcomes)
}

/// Interpreta `payoff` sobre una unica ruta y devuelve el ledger pathwise sin descontar
/// (equivalente Rust de "Cashflows"/`ScenarioEvaluator::evaluate`, PLAN_PRODUCTS.md §12 Fase 1/4,
/// pero sobre el IR compilado).
pub fn evaluate(payoff: &CompiledPayoff, path: &dyn ObservablePath) -> Vec<PathCashflow> {
    evaluate_with_events(payoff, path).0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::payoff::compile::compile;

    struct ConstantSpot(f64);
    impl ObservablePath for ConstantSpot {
        fn value_at(&self, _slot: usize, _time: f64) -> f64 {
            self.0
        }
    }

    /// Ruta con valores explicitos por `(slot, time)` (`slot` siempre 0 en estos tests: un unico
    /// observable). Permite ejercitar hits/no-hits/gaps de barrera sin depender de un modelo.
    struct StepPath(Vec<(f64, f64)>);
    impl ObservablePath for StepPath {
        fn value_at(&self, _slot: usize, time: f64) -> f64 {
            self.0
                .iter()
                .find(|(t, _)| (t - time).abs() < 1e-9)
                .unwrap_or_else(|| panic!("StepPath: sin valor para time={time}"))
                .1
        }
    }

    const CALL_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "AAPL_CALL_100",
        "contract": {
            "type": "when",
            "time": 1.0,
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "mul",
                    "left": {"type": "constant", "value": 1000.0},
                    "right": {
                        "type": "max",
                        "left": {
                            "type": "sub",
                            "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
                            "right": {"type": "constant", "value": 100.0}
                        },
                        "right": {"type": "constant", "value": 0.0}
                    }
                }
            }
        }
    }"#;

    #[test]
    fn call_in_the_money_pays_intrinsic_value_times_notional() {
        let payoff = compile(CALL_JSON).unwrap();
        let ledger = evaluate(&payoff, &ConstantSpot(120.0));
        assert_eq!(ledger, vec![PathCashflow { payment_time: 1.0, amount: 1000.0 * 20.0 }]);
    }

    #[test]
    fn call_out_of_the_money_pays_zero() {
        let payoff = compile(CALL_JSON).unwrap();
        let ledger = evaluate(&payoff, &ConstantSpot(80.0));
        assert_eq!(ledger, vec![PathCashflow { payment_time: 1.0, amount: 0.0 }]);
    }

    #[test]
    fn give_negates_every_cashflow_of_its_child() {
        let json = r#"{
            "schema": "engine.payoff/v1", "id": "x",
            "contract": {"type": "give", "child": {"type": "both", "children": [
                {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD",
                    "amount": {"type": "constant", "value": 10.0}}},
                {"type": "when", "time": 2.0, "child": {"type": "cashflow", "currency": "USD",
                    "amount": {"type": "constant", "value": 20.0}}}
            ]}}
        }"#;
        let payoff = compile(json).unwrap();
        let ledger = evaluate(&payoff, &ConstantSpot(0.0));
        assert_eq!(
            ledger,
            vec![
                PathCashflow { payment_time: 1.0, amount: -10.0 },
                PathCashflow { payment_time: 2.0, amount: -20.0 },
            ]
        );
    }

    #[test]
    #[should_panic(expected = "cursor de tiempo activo")]
    fn cashflow_without_enclosing_when_panics() {
        let json = r#"{
            "schema": "engine.payoff/v1", "id": "x",
            "contract": {"type": "cashflow", "currency": "USD", "amount": {"type": "constant", "value": 1.0}}
        }"#;
        let payoff = compile(json).unwrap();
        evaluate(&payoff, &ConstantSpot(0.0));
    }

    // Mismo contrato que docs/schema/engine.payoff/examples/barrier.json: up-and-in, barrera 120,
    // call europea 100 a T=1, monitorizado en [0.25, 0.5, 0.75, 1.0], settlement en la fecha
    // programada del 'when' interno (no AtHit) -- confirma ADR-P0-10 (Settlement no afecta a un
    // subarbol que ya trae su propio 'When').
    const UP_AND_IN_CALL_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "AAPL_UP_AND_IN_CALL_100_120",
        "contract": {
            "type": "trigger",
            "id": "UI",
            "monitoring_times": [0.25, 0.5, 0.75, 1.0],
            "condition": {
                "type": "greater_equal",
                "left": {"type": "current", "observable": "EQ.SPOT.AAPL"},
                "right": {"type": "constant", "value": 120.0}
            },
            "monitoring": "discrete",
            "settlement": "at_scheduled_payment",
            "priority": 0,
            "latch": true,
            "on_hit": {
                "type": "when",
                "time": 1.0,
                "child": {
                    "type": "cashflow",
                    "currency": "USD",
                    "amount": {
                        "type": "max",
                        "left": {
                            "type": "sub",
                            "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
                            "right": {"type": "constant", "value": 100.0}
                        },
                        "right": {"type": "constant", "value": 0.0}
                    }
                }
            },
            "on_miss": {"type": "zero"}
        }
    }"#;

    #[test]
    fn up_and_in_pays_underlying_when_barrier_is_hit() {
        let payoff = compile(UP_AND_IN_CALL_JSON).unwrap();
        let path = StepPath(vec![(0.25, 90.0), (0.5, 130.0), (0.75, 110.0), (1.0, 115.0)]);
        let (ledger, events) = evaluate_with_events(&payoff, &path);
        assert_eq!(ledger, vec![PathCashflow { payment_time: 1.0, amount: 15.0 }]);
        assert_eq!(payoff.event_slots, vec!["UI".to_string()]);
        assert_eq!(events[0], EventOutcome { occurred: true, first_hit_time: Some(0.5) });
    }

    #[test]
    fn up_and_in_pays_zero_when_barrier_is_never_hit_despite_gap_at_maturity() {
        let payoff = compile(UP_AND_IN_CALL_JSON).unwrap();
        // Nunca toca 120, ni siquiera con S_T=115 en el ultimo fixing (gap por debajo de la
        // barrera): el contrato vale Zero, no el intrinseco de la call subyacente.
        let path = StepPath(vec![(0.25, 90.0), (0.5, 95.0), (0.75, 110.0), (1.0, 115.0)]);
        let (ledger, events) = evaluate_with_events(&payoff, &path);
        assert_eq!(ledger, Vec::new());
        assert_eq!(events[0], EventOutcome { occurred: false, first_hit_time: None });
    }

    #[test]
    fn latch_keeps_barrier_active_even_if_spot_falls_back_below_it() {
        let payoff = compile(UP_AND_IN_CALL_JSON).unwrap();
        // Toca 120 en t=0.5 y luego cae muy por debajo -- 'latch=true' mantiene el hit.
        let path = StepPath(vec![(0.25, 90.0), (0.5, 125.0), (0.75, 80.0), (1.0, 70.0)]);
        let (ledger, events) = evaluate_with_events(&payoff, &path);
        // Intrinseco de la call es max(70-100,0)=0, pero el evento SI ocurrio (distinto de "never
        // hit"): confirma que 'latch' no se revierte, aunque el payoff resultante sea igual.
        assert_eq!(ledger, vec![PathCashflow { payment_time: 1.0, amount: 0.0 }]);
        assert!(events[0].occurred);
        assert_eq!(events[0].first_hit_time, Some(0.5));
    }

    // TP/SL (PLAN_PRODUCTS.md §4.3, ADR-P0-08 ultimo parrafo): dos triggers con EventId distinto,
    // cada uno con guarda 'Not(EventOccurred(el_otro))'. TAKE_PROFIT prioridad 10, STOP_LOSS 20:
    // en la misma fecha, si ambas condiciones fueran ciertas a la vez, TAKE_PROFIT gana porque se
    // evalua primero y su guarda deja a STOP_LOSS sin disparar ese mismo instante.
    const TP_SL_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "TP_SL",
        "contract": {
            "type": "both",
            "children": [
                {
                    "type": "trigger",
                    "id": "TAKE_PROFIT",
                    "monitoring_times": [0.25, 0.5, 0.75, 1.0],
                    "condition": {
                        "type": "all",
                        "operands": [
                            {"type": "greater_equal",
                                "left": {"type": "current", "observable": "EQ.SPOT.AAPL"},
                                "right": {"type": "constant", "value": 120.0}},
                            {"type": "not", "operand": {"type": "event_occurred", "event": "STOP_LOSS"}}
                        ]
                    },
                    "monitoring": "discrete", "settlement": "at_hit", "priority": 10, "latch": true,
                    "on_hit": {"type": "cashflow", "currency": "USD",
                        "amount": {"type": "event_value", "event": "TAKE_PROFIT", "observable": "EQ.SPOT.AAPL"}},
                    "on_miss": {"type": "zero"}
                },
                {
                    "type": "trigger",
                    "id": "STOP_LOSS",
                    "monitoring_times": [0.25, 0.5, 0.75, 1.0],
                    "condition": {
                        "type": "all",
                        "operands": [
                            {"type": "less_equal",
                                "left": {"type": "current", "observable": "EQ.SPOT.AAPL"},
                                "right": {"type": "constant", "value": 80.0}},
                            {"type": "not", "operand": {"type": "event_occurred", "event": "TAKE_PROFIT"}}
                        ]
                    },
                    "monitoring": "discrete", "settlement": "at_hit", "priority": 20, "latch": true,
                    "on_hit": {"type": "cashflow", "currency": "USD",
                        "amount": {"type": "event_value", "event": "STOP_LOSS", "observable": "EQ.SPOT.AAPL"}},
                    "on_miss": {"type": "zero"}
                }
            ]
        }
    }"#;

    #[test]
    fn take_profit_wins_when_both_conditions_are_true_on_the_same_date() {
        let payoff = compile(TP_SL_JSON).unwrap();
        // t=0.5: spot=125 dispara TAKE_PROFIT (>=120); no dispara STOP_LOSS porque 125 > 80.
        // Solo comprueba la exclusion mutua via la guarda cuando SI podrian coincidir: usamos un
        // spot que en la MISMA fecha satisface ambas condiciones brutas (imposible con un solo
        // nivel, asi que el escenario relevante es "TAKE_PROFIT ya latcheado bloquea STOP_LOSS en
        // fechas posteriores", ver el siguiente test) -- aqui solo fijamos que TAKE_PROFIT paga el
        // spot capturado.
        let path = StepPath(vec![(0.25, 100.0), (0.5, 125.0), (0.75, 40.0), (1.0, 40.0)]);
        let (ledger, events) = evaluate_with_events(&payoff, &path);
        assert_eq!(ledger, vec![PathCashflow { payment_time: 0.5, amount: 125.0 }]);
        assert_eq!(payoff.event_slots, vec!["TAKE_PROFIT".to_string(), "STOP_LOSS".to_string()]);
        assert_eq!(events[0], EventOutcome { occurred: true, first_hit_time: Some(0.5) });
        // STOP_LOSS: la guarda 'Not(EventOccurred(TAKE_PROFIT))' bloquea el disparo en t=0.75/1.0
        // (spot=40 <= 80) porque TAKE_PROFIT ya latcheo en t=0.5.
        assert_eq!(events[1], EventOutcome { occurred: false, first_hit_time: None });
    }

    #[test]
    fn stop_loss_fires_when_take_profit_never_triggers() {
        let payoff = compile(TP_SL_JSON).unwrap();
        let path = StepPath(vec![(0.25, 100.0), (0.5, 90.0), (0.75, 75.0), (1.0, 60.0)]);
        let (ledger, events) = evaluate_with_events(&payoff, &path);
        assert_eq!(ledger, vec![PathCashflow { payment_time: 0.75, amount: 75.0 }]);
        assert_eq!(events[0], EventOutcome { occurred: false, first_hit_time: None });
        assert_eq!(events[1], EventOutcome { occurred: true, first_hit_time: Some(0.75) });
    }

    // Brownian bridge (PLAN_PRODUCTS.md §4.2, Fase 6).

    #[test]
    fn bridge_crossing_probability_matches_hand_computed_value() {
        // exp(-2*ln(120/100)*ln(120/110) / (0.3^2*0.5)) -- calculado a mano con ln(1.2)=0.18232,
        // ln(120/110)=0.087011.
        let p = bridge_crossing_probability(100.0, 110.0, 120.0, 0.3, 0.5).unwrap();
        let expected = (-2.0 * (120.0_f64 / 100.0).ln() * (120.0_f64 / 110.0).ln() / (0.3 * 0.3 * 0.5)).exp();
        assert!((p - expected).abs() < 1e-12);
        assert!(p > 0.0 && p < 1.0, "p={p}");
    }

    #[test]
    fn bridge_crossing_probability_is_none_for_non_positive_or_degenerate_inputs() {
        assert!(bridge_crossing_probability(100.0, 110.0, 120.0, 0.3, 0.0).is_none()); // dt=0
        assert!(bridge_crossing_probability(100.0, 110.0, 120.0, 0.0, 0.5).is_none()); // sigma=0
        assert!(bridge_crossing_probability(-1.0, 110.0, 120.0, 0.3, 0.5).is_none()); // a<0
    }

    #[test]
    fn previous_monitoring_time_finds_the_immediate_predecessor() {
        let times = [0.25, 0.5, 0.75, 1.0];
        assert_eq!(previous_monitoring_time(&times, 0.25), None);
        assert_eq!(previous_monitoring_time(&times, 0.5), Some(0.25));
        assert_eq!(previous_monitoring_time(&times, 1.0), Some(0.75));
        assert_eq!(previous_monitoring_time(&times, 2.0), Some(1.0));
    }

    struct FixedStepPathWithSigma {
        times: Vec<f64>,
        values: Vec<f64>,
        sigma: f64,
    }
    impl ObservablePath for FixedStepPathWithSigma {
        fn value_at(&self, _slot: usize, time: f64) -> f64 {
            self.times
                .iter()
                .position(|&t| (t - time).abs() < 1e-9)
                .map(|i| self.values[i])
                .unwrap_or_else(|| panic!("FixedStepPathWithSigma: sin valor para time={time}"))
        }
        fn volatility(&self, _slot: usize) -> f64 {
            self.sigma
        }
    }

    const CONTINUOUS_UP_AND_IN_JSON: &str = r#"{
        "schema": "engine.payoff/v1", "id": "UI_CONT",
        "contract": {
            "type": "trigger", "id": "UI",
            "monitoring_times": [0.5, 1.0],
            "condition": {"type": "greater_equal",
                "left": {"type": "current", "observable": "EQ.SPOT.XYZ"},
                "right": {"type": "constant", "value": 120.0}},
            "monitoring": "continuous_approximation", "settlement": "at_hit", "priority": 0, "latch": true,
            "on_hit": {"type": "cashflow", "currency": "USD", "amount": {"type": "constant", "value": 1.0}},
            "on_miss": {"type": "zero"}
        }
    }"#;

    #[test]
    fn continuous_approximation_hit_rate_over_many_seeds_matches_the_analytic_crossing_probability() {
        // Ambos extremos (t=0.5 -> 100, t=1.0 -> 110) estan por debajo de la barrera 120: el
        // chequeo discreto nunca dispara solo, asi que CUALQUIER hit observado viene de la
        // correccion de bridge. Repetir con muchos bridge_seed distintos sobre la MISMA ruta
        // aisla el mecanismo de muestreo (RNG + formula) de la simulacion GBM real.
        let payoff = compile(CONTINUOUS_UP_AND_IN_JSON).unwrap();
        let (a, b, sigma, barrier, dt) = (100.0, 110.0, 0.3, 120.0, 0.5);
        let path = FixedStepPathWithSigma { times: vec![0.5, 1.0], values: vec![a, b], sigma };
        let expected_p = bridge_crossing_probability(a, b, barrier, sigma, dt).unwrap();

        let n_trials = 20_000u64;
        let hits = (0..n_trials)
            .filter(|&trial| evaluate_with_events_seeded(&payoff, &path, trial).1[0].occurred)
            .count();
        let observed_rate = hits as f64 / n_trials as f64;
        assert!(
            (observed_rate - expected_p).abs() < 0.02,
            "observed={observed_rate} expected={expected_p} (n_trials={n_trials})"
        );
    }

    #[test]
    fn continuous_approximation_never_bridges_before_the_triggers_own_first_monitoring_time() {
        // Limitacion documentada (doc-comment del modulo): sin extremo previo PROPIO del
        // Trigger, la correccion de bridge no se aplica -- un solo monitoring_time nunca dispara
        // por bridge aunque la probabilidad analitica seria alta.
        let json = CONTINUOUS_UP_AND_IN_JSON.replace(r#""monitoring_times": [0.5, 1.0],"#, r#""monitoring_times": [1.0],"#);
        let payoff = compile(&json).unwrap();
        let path = FixedStepPathWithSigma { times: vec![1.0], values: vec![110.0], sigma: 0.3 };
        for trial in 0..1_000u64 {
            let (_ledger, events) = evaluate_with_events_seeded(&payoff, &path, trial);
            assert!(!events[0].occurred, "no deberia haber bridge sin un extremo previo (trial={trial})");
        }
    }

    #[test]
    fn continuous_approximation_still_detects_an_exact_discrete_touch_without_needing_the_bridge() {
        // Si el extremo YA toca/supera la barrera en un instante de monitorizacion, el chequeo
        // discreto normal (identico al de Monitoring::Discrete) lo detecta sin necesitar sorteo:
        // el resultado es determinista para CUALQUIER bridge_seed.
        let payoff = compile(CONTINUOUS_UP_AND_IN_JSON).unwrap();
        let path = FixedStepPathWithSigma { times: vec![0.5, 1.0], values: vec![125.0, 110.0], sigma: 0.3 };
        for trial in 0..50u64 {
            let (ledger, events) = evaluate_with_events_seeded(&payoff, &path, trial);
            assert_eq!(events[0], EventOutcome { occurred: true, first_hit_time: Some(0.5) });
            assert_eq!(ledger, vec![PathCashflow { payment_time: 0.5, amount: 1.0 }]);
        }
    }
}
