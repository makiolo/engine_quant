//! Longstaff-Schwartz (PLAN_PRODUCTS.md §10, Fase 9): resuelve la decision de ejercicio de un
//! `ContractOp::Exercise` sobre un LOTE COMPLETO de rutas ya simuladas -- a diferencia de
//! `eval::resolve_trigger_states` (que resuelve un `Trigger` ruta a ruta, sin necesitar ver
//! ninguna otra), la decision optima de ejercicio en una fecha depende de una estimacion de
//! `E_Q[continuacion | S_t]` que solo tiene sentido con la seccion transversal de MUCHAS rutas en
//! esa misma fecha -- de ahi que este modulo viva aparte de `eval` y se ejecute UNA VEZ por
//! `Exercise`, no una vez por ruta.
//!
//! **Alcance de Fase 9** (documentado, no oculto -- PLAN_PRODUCTS.md §16): exactamente UN
//! `ContractOp::Exercise` por programa (`find_single_exercise_node` rechaza 0 o >1 -- combinar
//! varios derechos de ejercicio independientes, o un `Exercise` anidado dentro de la
//! `continuation` de otro, requeriria una induccion hacia atras conjunta que esta version no
//! implementa) y un unico observable (misma simplificacion que `api::check_single_observable`,
//! Fase 5): la base de regresion es cuadratica en ESE observable, `[1, S, S^2]` (Longstaff-Schwartz
//! 2001, la base mas simple que captura convexidad -- suficiente para vanilla put/call americana/
//! bermuda, que es el caso de aceptacion de esta fase).
//!
//! **Sin look-ahead** (criterio de aceptacion explicito de Fase 9): en cada fecha de decision `t`
//! (recorridas de la MAS TARDIA a la MAS TEMPRANA), la decision de cada ruta se basa UNICAMENTE en
//! `exercise_value(ruta, t)` (observable en `t`) y en `continuation_estimate(ruta, t)` -- el valor
//! AJUSTADO por la regresion, funcion solo de `S_t`, nunca en el cashflow futuro REAL que esa
//! misma ruta acabo realizando (eso seria mirar el futuro para decidir el presente). El cashflow
//! futuro real de cada ruta solo se usa como VARIABLE DEPENDIENTE de la regresion (`y`), agregado
//! con las demas rutas -- exactamente el mecanismo que hace de Longstaff-Schwartz un estimador
//! consistente y no un oraculo.
//!
//! **Reproducibilidad**: esta funcion es determinista dado `paths`/`trigger_states_by_path`
//! (ambos ya fijados por el RNG sembrado de quien simula, ver `api::price_payoff_exercise_gbm_q`)
//! -- no consume ningun generador aleatorio propio, la regresion de minimos cuadrados no tiene
//! grados de libertad de aleatoriedad.

use super::eval::{eval_contract, eval_scalar, EventStateResolved, ObservablePath, PathCashflow};
use super::ir::{CompiledPayoff, ContractOp};

/// Los cuatro campos de un `ContractOp::Exercise` ya localizado (ver `find_single_exercise_node`).
#[derive(Debug, Clone)]
pub(crate) struct ExerciseNode {
    pub(crate) event: usize,
    pub(crate) dates: Vec<f64>,
    pub(crate) exercise_value: usize,
    pub(crate) continuation: usize,
}

/// Localiza el UNICO `ContractOp::Exercise` de `payoff` (ver el doc-comment del modulo sobre el
/// alcance de Fase 9). `Err` si no hay ninguno o si hay mas de uno -- preflight, nunca se llega a
/// simular una ruta con un programa que esta funcion no pueda resolver.
pub(crate) fn find_single_exercise_node(payoff: &CompiledPayoff) -> Result<ExerciseNode, String> {
    let mut found: Option<ExerciseNode> = None;
    for op in &payoff.contract_ops {
        if let ContractOp::Exercise { event, dates, exercise_value, continuation } = op {
            if found.is_some() {
                return Err(
                    "payoff: mas de un nodo Exercise en el mismo contrato no soportado en Fase 9 (Longstaff-Schwartz \
                     de un unico derecho de ejercicio, PLAN_PRODUCTS.md §16)"
                        .to_string(),
                );
            }
            found = Some(ExerciseNode {
                event: *event,
                dates: dates.clone(),
                exercise_value: *exercise_value,
                continuation: *continuation,
            });
        }
    }
    found.ok_or_else(|| "payoff: el contrato no contiene ningun nodo Exercise".to_string())
}

/// Coeficientes `(a, b, c)` de `y ~ a + b*x + c*x^2` ajustados por minimos cuadrados ordinarios
/// (ecuaciones normales 3x3, resueltas por eliminacion gaussiana con pivoteo parcial -- la base es
/// fija y pequena, no hace falta nada mas sofisticado). `None` si el sistema es singular (por
/// ejemplo, todos los `x` identicos) o si hay menos de 3 puntos.
fn fit_quadratic(x: &[f64], y: &[f64]) -> Option<(f64, f64, f64)> {
    let n = x.len();
    if n < 3 {
        return None;
    }
    // Ecuaciones normales de [1, x, x^2]: A^T A coef = A^T y, montadas directamente (evita
    // materializar la matriz de diseno n x 3, irrelevante en tamano pero mas simple de leer).
    let (mut s0, mut s1, mut s2, mut s3, mut s4) = (0.0, 0.0, 0.0, 0.0, 0.0);
    let (mut t0, mut t1, mut t2) = (0.0, 0.0, 0.0);
    for i in 0..n {
        let (xi, yi) = (x[i], y[i]);
        let (xi2, xi3, xi4) = (xi * xi, xi * xi * xi, xi * xi * xi * xi);
        s0 += 1.0;
        s1 += xi;
        s2 += xi2;
        s3 += xi3;
        s4 += xi4;
        t0 += yi;
        t1 += xi * yi;
        t2 += xi2 * yi;
    }
    let mut m = [[s0, s1, s2, t0], [s1, s2, s3, t1], [s2, s3, s4, t2]];
    solve_3x3(&mut m)
}

/// Elimacion gaussiana con pivoteo parcial sobre una matriz aumentada 3x4 (`m[i] = [a,b,c | rhs]`).
/// `None` si es singular (pivote numericamente cero) -- el llamante trata "no se puede ajustar"
/// como "no hay evidencia suficiente para justificar ejercicio anticipado" (nunca extrapola).
///
/// Indexado directo (no iteradores) deliberado: la eliminacion de la fila pivote actualiza OTRAS
/// filas leyendo la pivote a la vez (`m[row][k] -= factor * m[col][k]`), lo que un iterador sobre
/// una sola fila no puede expresar sin duplicar la fila pivote en un buffer aparte -- para una
/// matriz 3x4 fija, el indexado es mas claro que esa alternativa.
#[allow(clippy::needless_range_loop)]
fn solve_3x3(m: &mut [[f64; 4]; 3]) -> Option<(f64, f64, f64)> {
    const EPS: f64 = 1e-10;
    for col in 0..3 {
        let pivot_row = (col..3).max_by(|&a, &b| m[a][col].abs().partial_cmp(&m[b][col].abs()).unwrap())?;
        if m[pivot_row][col].abs() < EPS {
            return None;
        }
        m.swap(col, pivot_row);
        let pivot = m[col][col];
        for k in col..4 {
            m[col][k] /= pivot;
        }
        for row in 0..3 {
            if row == col {
                continue;
            }
            let factor = m[row][col];
            for k in col..4 {
                m[row][k] -= factor * m[col][k];
            }
        }
    }
    Some((m[0][3], m[1][3], m[2][3]))
}

/// Diagnostico de UNA fecha de decision, exportado junto al precio (PLAN_PRODUCTS.md §10:
/// "diagnostico de regresion y politica de ejercicio exportable") -- ver `api::ExercisePolicy`.
#[derive(Debug, Clone, Copy)]
pub struct ExerciseDateDiagnostic {
    pub date: f64,
    pub n_in_the_money: u64,
    /// `None` si en `date` no hubo (al menos) 3 rutas in-the-money para ajustar la regresion --
    /// en ese caso ninguna ruta ejercita en `date` (ver `fit_quadratic`/`solve_3x3`).
    pub regression_coeffs: Option<(f64, f64, f64)>,
    /// Fraccion de rutas in-the-money que decidieron ejercitar en `date` (0.0 si
    /// `regression_coeffs` es `None`).
    pub exercised_fraction: f64,
}

/// Resuelve, para cada ruta de `paths`, en que fecha de `node.dates` (si alguna) ejercita, via
/// Longstaff-Schwartz (induccion hacia atras, regresion cuadratica sobre las rutas in-the-money en
/// cada fecha -- ver el doc-comment del modulo). Devuelve `(decisions, diagnostics)`:
/// `decisions[path]` es `Some(fecha)`/`None` (nunca ejercito, cae en `node.continuation`);
/// `diagnostics` tiene una entrada por fecha de `node.dates`, en el MISMO orden (ascendente).
///
/// `trigger_states_by_path[path]` es el resultado de `eval::resolve_trigger_states` para esa ruta
/// (estados de cualquier `Trigger` del arbol, irrelevantes para la decision de ejercicio en si
/// pero necesarios para evaluar `node.continuation` si esta referencia un evento) -- el slot
/// `node.event` de ese vector se ignora (todavia no resuelto en el momento en que se le pasa a
/// esta funcion).
pub(crate) fn resolve_exercise_decisions<P: ObservablePath>(
    payoff: &CompiledPayoff,
    node: &ExerciseNode,
    paths: &[P],
    trigger_states_by_path: &[Vec<EventStateResolved>],
    r: f64,
) -> (Vec<Option<f64>>, Vec<ExerciseDateDiagnostic>) {
    let n = paths.len();
    debug_assert_eq!(trigger_states_by_path.len(), n);

    // "realized[path]": el ledger que esa ruta recibiria bajo la politica optima determinada HASTA
    // AHORA (empezando, antes de procesar ninguna fecha, por 'nunca ejercer' == la propia
    // 'continuation'). Se sobrescribe con un unico cashflow en cuanto una fecha decide ejercer.
    let mut realized: Vec<Vec<PathCashflow>> = (0..n)
        .map(|i| {
            let mut out = Vec::new();
            eval_contract(payoff, node.continuation, None, &paths[i], &trigger_states_by_path[i], &mut out);
            out
        })
        .collect();
    let mut decisions: Vec<Option<f64>> = vec![None; n];
    let mut diagnostics: Vec<ExerciseDateDiagnostic> = Vec::with_capacity(node.dates.len());

    for &t in node.dates.iter().rev() {
        let exercise_value: Vec<f64> = (0..n)
            .map(|i| eval_scalar(payoff, node.exercise_value, Some(t), &paths[i], &trigger_states_by_path[i]))
            .collect();
        let underlying: Vec<f64> = (0..n).map(|i| paths[i].value_at(0, t)).collect();
        // Objetivo de la regresion: el ledger 'realized' de cada ruta (ya fijado por decisiones en
        // fechas POSTERIORES, procesadas antes por recorrer 'dates' en reversa) descontado a `t`.
        let continuation_target: Vec<f64> = realized
            .iter()
            .map(|cfs| cfs.iter().map(|cf| cf.amount * (-r * (cf.payment_time - t)).exp()).sum())
            .collect();

        let itm: Vec<usize> = (0..n).filter(|&i| exercise_value[i] > 0.0).collect();
        let coeffs = if itm.len() >= 3 {
            let xs: Vec<f64> = itm.iter().map(|&i| underlying[i]).collect();
            let ys: Vec<f64> = itm.iter().map(|&i| continuation_target[i]).collect();
            fit_quadratic(&xs, &ys)
        } else {
            None
        };

        let mut exercised_here = 0u64;
        if let Some((a, b, c)) = coeffs {
            for &i in &itm {
                let s = underlying[i];
                let continuation_estimate = a + b * s + c * s * s;
                if exercise_value[i] >= continuation_estimate {
                    realized[i] = vec![PathCashflow { payment_time: t, amount: exercise_value[i] }];
                    decisions[i] = Some(t);
                    exercised_here += 1;
                }
            }
        }

        diagnostics.push(ExerciseDateDiagnostic {
            date: t,
            n_in_the_money: itm.len() as u64,
            regression_coeffs: coeffs,
            exercised_fraction: if itm.is_empty() { 0.0 } else { exercised_here as f64 / itm.len() as f64 },
        });
    }

    diagnostics.reverse(); // se construyo en orden reverso (mas tardia primero); exponer ascendente.
    (decisions, diagnostics)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::payoff::compile::compile;

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

    #[test]
    fn fit_quadratic_recovers_exact_coefficients_from_noiseless_points() {
        // y = 2 - 3x + 0.5x^2, evaluado exactamente en 5 puntos -- minimos cuadrados sobre datos
        // sin ruido debe recuperar los coeficientes casi exactos.
        let a_true = 2.0_f64;
        let b_true = -3.0_f64;
        let c_true = 0.5_f64;
        let xs = vec![-2.0, -1.0, 0.0, 1.0, 2.0, 3.0];
        let ys: Vec<f64> = xs.iter().map(|&x| a_true + b_true * x + c_true * x * x).collect();
        let (a, b, c) = fit_quadratic(&xs, &ys).expect("deberia poder ajustar");
        assert!((a - a_true).abs() < 1e-8, "a={a}");
        assert!((b - b_true).abs() < 1e-8, "b={b}");
        assert!((c - c_true).abs() < 1e-8, "c={c}");
    }

    #[test]
    fn fit_quadratic_returns_none_with_fewer_than_three_points() {
        assert!(fit_quadratic(&[1.0, 2.0], &[1.0, 2.0]).is_none());
    }

    #[test]
    fn fit_quadratic_returns_none_when_all_x_are_identical() {
        // Columna [x, x^2] degenerada (constante): sistema singular.
        assert!(fit_quadratic(&[5.0, 5.0, 5.0, 5.0], &[1.0, 2.0, 3.0, 4.0]).is_none());
    }

    // Put bermuda de dos fechas (0.5, 1.0), continuation = put europea a T=1 (strike 100). Rutas
    // hechas a mano, deliberadamente "in the money profundo y quieto" en 0.5 para que la decision
    // de ejercer sea inequivoca sin depender de la calidad estadistica de la regresion (eso se
    // prueba con Monte Carlo real en api.rs).
    const PUT_BERMUDA_JSON: &str = r#"{
        "schema": "engine.payoff/v1", "id": "PUT_BERMUDA",
        "contract": {
            "type": "exercise", "id": "EX",
            "dates": [0.5, 1.0],
            "exercise_value": {"type": "max",
                "left": {"type": "sub",
                    "left": {"type": "constant", "value": 100.0},
                    "right": {"type": "current", "observable": "EQ.SPOT.XYZ"}},
                "right": {"type": "constant", "value": 0.0}},
            "continuation": {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD",
                "amount": {"type": "max",
                    "left": {"type": "sub",
                        "left": {"type": "constant", "value": 100.0},
                        "right": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": 1.0}},
                    "right": {"type": "constant", "value": 0.0}}}}
        }
    }"#;

    #[test]
    fn deep_itm_paths_that_stay_deep_itm_exercise_early_when_favorable() {
        let payoff = compile(PUT_BERMUDA_JSON).unwrap();
        let node = find_single_exercise_node(&payoff).unwrap();
        // Diez rutas deep ITM en ambas fechas, con una pequena dispersion en el spot (5..14 en
        // t=0.5, 90..99 en t=1.0, correlada) -- suficiente variacion para que la regresion
        // cuadratica no sea singular (ver 'fit_quadratic_returns_none_when_all_x_are_identical'),
        // pero manteniendo el mismo argumento economico: exercise_value en 0.5 (86..95) domina con
        // holgura el valor de continuacion, que en 0.5 nunca puede superar ~10 (el maximo
        // intrinseco posible en 1.0 dentro de este conjunto) -- la politica optima ejerce en 0.5
        // en TODAS las rutas, sin depender de los coeficientes exactos que ajuste la regresion.
        let paths: Vec<StepPath> = (0..10)
            .map(|i| StepPath(vec![(0.5, 5.0 + i as f64), (1.0, 90.0 + i as f64)]))
            .collect();
        let trigger_states: Vec<Vec<EventStateResolved>> =
            (0..paths.len()).map(|_| vec![EventStateResolved::new(payoff.observable_slots.len())]).collect();

        let (decisions, diagnostics) = resolve_exercise_decisions(&payoff, &node, &paths, &trigger_states, 0.05);
        assert!(decisions.iter().all(|d| *d == Some(0.5)), "decisions={decisions:?}");
        assert_eq!(diagnostics.len(), 2);
        assert_eq!(diagnostics[0].date, 0.5);
        assert_eq!(diagnostics[1].date, 1.0);
    }

    #[test]
    fn out_of_the_money_paths_never_exercise() {
        let payoff = compile(PUT_BERMUDA_JSON).unwrap();
        let node = find_single_exercise_node(&payoff).unwrap();
        // Spot siempre muy por encima del strike: exercise_value=0 en ambas fechas -> nunca ITM.
        let paths: Vec<StepPath> = (0..10).map(|_| StepPath(vec![(0.5, 150.0), (1.0, 160.0)])).collect();
        let trigger_states: Vec<Vec<EventStateResolved>> =
            (0..paths.len()).map(|_| vec![EventStateResolved::new(payoff.observable_slots.len())]).collect();

        let (decisions, diagnostics) = resolve_exercise_decisions(&payoff, &node, &paths, &trigger_states, 0.05);
        assert!(decisions.iter().all(|d| d.is_none()), "decisions={decisions:?}");
        assert!(diagnostics.iter().all(|d| d.n_in_the_money == 0));
    }
}
