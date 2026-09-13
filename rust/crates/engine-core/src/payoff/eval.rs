//! Interprete pathwise escalar de referencia sobre `CompiledPayoff` (PLAN_PRODUCTS.md §12 Fase 5:
//! "ejecucion pathwise escalar de referencia en Rust"). Equivalente Rust, sobre el IR compilado,
//! de `engine::payoff::ScenarioEvaluator::evaluate` en C++ (que interpreta directamente el AST
//! con punteros compartidos): recorre `payoff.root` recursivamente, resolviendo cada nodo por
//! indice en el `Vec` de su categoria en vez de doble dispatch virtual.
//!
//! No descuenta ni decide medida probabilistica (igual que `ScenarioEvaluator` en C++, §5.2):
//! solo produce el ledger pathwise sin agregar para UNA ruta ya simulada. Quien llama
//! (`crate::models::gbm`/`crate::mc`) descuenta bajo Q y agrega sobre muchas rutas.

use super::ir::{CompiledPayoff, ContractOp, PredicateOp, ScalarOp};

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
}

/// Evalua `payoff.scalar_ops[idx]`. `cursor` es el "instante activo" de ADR-P0-08
/// (PLAN_PRODUCTS.md): solo `ContractOp::When` lo fija; `Current` lo requiere.
fn eval_scalar(payoff: &CompiledPayoff, idx: usize, cursor: Option<f64>, path: &dyn ObservablePath) -> f64 {
    match &payoff.scalar_ops[idx] {
        ScalarOp::Constant(v) => *v,
        ScalarOp::Fixing { observable, time } => path.value_at(*observable, *time),
        ScalarOp::Current { observable } => {
            let t = cursor.expect("payoff: 'current' sin cursor de tiempo activo (falta un 'when' envolvente)");
            path.value_at(*observable, t)
        }
        ScalarOp::Add(l, r) => eval_scalar(payoff, *l, cursor, path) + eval_scalar(payoff, *r, cursor, path),
        ScalarOp::Sub(l, r) => eval_scalar(payoff, *l, cursor, path) - eval_scalar(payoff, *r, cursor, path),
        ScalarOp::Mul(l, r) => eval_scalar(payoff, *l, cursor, path) * eval_scalar(payoff, *r, cursor, path),
        ScalarOp::Div(l, r) => {
            let denominator = eval_scalar(payoff, *r, cursor, path);
            assert!(denominator != 0.0, "payoff: division por cero");
            eval_scalar(payoff, *l, cursor, path) / denominator
        }
        ScalarOp::Neg(x) => -eval_scalar(payoff, *x, cursor, path),
        ScalarOp::Abs(x) => eval_scalar(payoff, *x, cursor, path).abs(),
        ScalarOp::Exp(x) => eval_scalar(payoff, *x, cursor, path).exp(),
        ScalarOp::Log(x) => eval_scalar(payoff, *x, cursor, path).ln(),
        ScalarOp::Pow(base, exponent) => {
            eval_scalar(payoff, *base, cursor, path).powf(eval_scalar(payoff, *exponent, cursor, path))
        }
        ScalarOp::Min(l, r) => eval_scalar(payoff, *l, cursor, path).min(eval_scalar(payoff, *r, cursor, path)),
        ScalarOp::Max(l, r) => eval_scalar(payoff, *l, cursor, path).max(eval_scalar(payoff, *r, cursor, path)),
        ScalarOp::Clamp { value, low, high } => {
            let v = eval_scalar(payoff, *value, cursor, path);
            let lo = eval_scalar(payoff, *low, cursor, path);
            let hi = eval_scalar(payoff, *high, cursor, path);
            v.max(lo).min(hi)
        }
    }
}

/// Evalua `payoff.predicate_ops[idx]`. `All`/`Any` cortocircuitan (ADR-P0-03), igual que exige el
/// AST de autoria en C++.
fn eval_predicate(payoff: &CompiledPayoff, idx: usize, cursor: Option<f64>, path: &dyn ObservablePath) -> bool {
    match &payoff.predicate_ops[idx] {
        PredicateOp::Greater(l, r) => eval_scalar(payoff, *l, cursor, path) > eval_scalar(payoff, *r, cursor, path),
        PredicateOp::Less(l, r) => eval_scalar(payoff, *l, cursor, path) < eval_scalar(payoff, *r, cursor, path),
        PredicateOp::GreaterEqual(l, r) => {
            eval_scalar(payoff, *l, cursor, path) >= eval_scalar(payoff, *r, cursor, path)
        }
        PredicateOp::LessEqual(l, r) => {
            eval_scalar(payoff, *l, cursor, path) <= eval_scalar(payoff, *r, cursor, path)
        }
        PredicateOp::Eq { left, right, tolerance } => {
            (eval_scalar(payoff, *left, cursor, path) - eval_scalar(payoff, *right, cursor, path)).abs()
                <= *tolerance
        }
        PredicateOp::All(operands) => operands.iter().all(|&i| eval_predicate(payoff, i, cursor, path)),
        PredicateOp::Any(operands) => operands.iter().any(|&i| eval_predicate(payoff, i, cursor, path)),
        PredicateOp::Not(operand) => !eval_predicate(payoff, *operand, cursor, path),
        PredicateOp::Between { value, low, high, low_inclusive, high_inclusive } => {
            let v = eval_scalar(payoff, *value, cursor, path);
            let lo = eval_scalar(payoff, *low, cursor, path);
            let hi = eval_scalar(payoff, *high, cursor, path);
            let low_ok = if *low_inclusive { v >= lo } else { v > lo };
            let high_ok = if *high_inclusive { v <= hi } else { v < hi };
            low_ok && high_ok
        }
    }
}

/// Evalua `payoff.contract_ops[idx]`, acumulando cashflows en `out`. `Give`/`Scale` aplican su
/// transformacion a TODOS los cashflows que su hijo produjo (ADR-P0-02), no solo al primero.
fn eval_contract(
    payoff: &CompiledPayoff,
    idx: usize,
    cursor: Option<f64>,
    path: &dyn ObservablePath,
    out: &mut Vec<PathCashflow>,
) {
    match &payoff.contract_ops[idx] {
        ContractOp::Zero => {}
        ContractOp::Cashflow { amount } => {
            let t = cursor.expect("payoff: 'cashflow' sin cursor de tiempo activo (falta un 'when' envolvente)");
            out.push(PathCashflow { payment_time: t, amount: eval_scalar(payoff, *amount, cursor, path) });
        }
        ContractOp::Give(child) => {
            let start = out.len();
            eval_contract(payoff, *child, cursor, path, out);
            for cf in &mut out[start..] {
                cf.amount = -cf.amount;
            }
        }
        ContractOp::Both(children) => {
            for &child in children {
                eval_contract(payoff, child, cursor, path, out);
            }
        }
        ContractOp::Scale { factor, child } => {
            let f = eval_scalar(payoff, *factor, cursor, path);
            let start = out.len();
            eval_contract(payoff, *child, cursor, path, out);
            for cf in &mut out[start..] {
                cf.amount *= f;
            }
        }
        ContractOp::If { condition, if_true, if_false } => {
            if eval_predicate(payoff, *condition, cursor, path) {
                eval_contract(payoff, *if_true, cursor, path, out);
            } else {
                eval_contract(payoff, *if_false, cursor, path, out);
            }
        }
        ContractOp::When { time, child } => eval_contract(payoff, *child, Some(*time), path, out),
    }
}

/// Interpreta `payoff` sobre una unica ruta y devuelve el ledger pathwise sin descontar
/// (equivalente Rust de "Cashflows"/`ScenarioEvaluator::evaluate`, PLAN_PRODUCTS.md §12 Fase 1/4,
/// pero sobre el IR compilado).
pub fn evaluate(payoff: &CompiledPayoff, path: &dyn ObservablePath) -> Vec<PathCashflow> {
    let mut out = Vec::new();
    eval_contract(payoff, payoff.root, None, path, &mut out);
    out
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
}
