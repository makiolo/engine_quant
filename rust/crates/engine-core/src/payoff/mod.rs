//! Fase 5 (PLAN_PRODUCTS.md §8, §12 "Fase 5 -- `CompiledPayoff` y kernel Rust Q"):
//! representacion plana versionada del AST de payoff (la autoria del AST vive en C++/Python, ver
//! `cpp/engine/include/engine/payoff/`) + interprete pathwise escalar de referencia, ambos en
//! Rust puro, mas los modelos/agregacion Monte Carlo bajo Q que lo consumen (`crate::models::gbm`,
//! `crate::mc`).
//!
//! Alcance (Fase 5 + Fase 6 "barreras bajo Q y exposicion", PLAN_PRODUCTS.md §12; Fase 2 de
//! PLAN_IMPROVE_NOTEBOOK.md anade `Average`/`RunningMin`/`RunningMax`):
//! `Zero`/`Cashflow`/`Give`/`Both`/`Scale`/`If`/`When`/`Trigger` (`Monitoring::Discrete` Y
//! `ContinuousApproximation` con un patron de barrera reconocido, ver `eval`), aritmetica escalar
//! completa (`Add`/`Sub`/.../`Clamp`/`EventValue`/`Average`/`RunningMin`/`RunningMax`) y predicados
//! de comparacion/`Between`/`All`/`Any`/`Not`/`EventOccurred`. La resolucion de `Trigger` replica
//! en Rust (`eval::resolve_trigger_states`) la misma semantica de dos pasadas que
//! `ScenarioEvaluator` en C++ (ADR-P0-03/08/10): primer hit, latch, prioridad/orden de `EventId` y
//! "instante activo", mas la correccion de Brownian bridge para monitorizacion continua (ver el
//! doc-comment de `eval`).
//!
//! **`Average`/`RunningMin`/`RunningMax` (PLAN_IMPROVE_NOTEBOOK.md Fase 2)**: pese a que el nombre
//! "running" sugiere estado mutable acumulado DURANTE la simulacion, estos tres nodos son
//! agregados PUROS -- ninguno necesita (ni existe) un acumulador por ruta a lo largo de los pasos
//! de tiempo. `Average` lleva su propio `schedule`/`weights` fijo y conocido en preflight (misma
//! forma que `ContractOp::Trigger::monitoring_times`/`ContractOp::Exercise::dates`, y **misma
//! forma exacta que el AST de autoria C++/Python/`docs/schema` ya establecido** --
//! `schedule`/`weights` es una suma PONDERADA, no una media con divisor implicito, ver el
//! doc-comment de `ScalarOp::Average` en `ir.rs`): su `schedule` entra en la union de
//! `required_times()` que el modelo simula de una vez antes de interpretar el programa.
//! `RunningMin`/`RunningMax` NO llevan schedule propio (mismo AST de autoria ya establecido: solo
//! `observable`) -- reducen en evaluacion sobre `CompiledPayoff::required_times()` filtrado a los
//! instantes `<=` el cursor activo de ADR-P0-08 (unico conjunto de instantes que este motor
//! conoce en preflight, a falta de la `MarketPath` poblada libremente que usa el evaluador
//! deterministico C++ para la misma semantica -- ver el doc-comment de `ScalarOp::RunningMin` en
//! `ir.rs` para el razonamiento completo, incluida la implicacion practica para quien autora un
//! lookback con monitorizacion fina). En los tres casos, `eval::eval_scalar`/
//! `sensitivity::eval_scalar` resuelven con acceso aleatorio a la ruta ya materializada
//! (`ObservablePath::value_at`), igual de "puros" que cualquier otro `ScalarOp`.
//!
//! `EventTime`/`DiscountFactor`/`FxConversion`/`Parameter`/`Before`/`After` quedan fuera (friccion
//! DISTINTA a la de `Average`/`RunningMin`/`RunningMax`, no cerrada por la Fase 2 de
//! PLAN_IMPROVE_NOTEBOOK.md): requieren un observable de mercado que ningun modelo Q de esta fase
//! genera, igual que el `ValidationVisitor`/`ScenarioEvaluator` de C++ tampoco los evalua todavia
//! -- `compile::compile` rechaza esos nodos en preflight, nunca ejecutando parcialmente el
//! contrato.
//!
//! **`Exercise` (Fase 9, PLAN_PRODUCTS.md §10)**: soportado, pero por un camino DISTINTO al resto
//! de `ContractOp` -- `eval::eval_contract` solo sabe APLICAR una decision de ejercicio ya
//! resuelta (nunca la toma), y esa decision se resuelve por Longstaff-Schwartz sobre el LOTE
//! completo de rutas (`lsm::resolve_exercise_decisions`), no ruta a ruta como `Trigger`. Ver el
//! doc-comment de `lsm` y `api::price_payoff_exercise_gbm_q` (la unica funcion publica que sabe
//! orquestar las dos pasadas).
//!
//! **Como cruza la frontera cxx**: `CompiledPayoff` en si mismo NUNCA se serializa a traves de
//! `cxx` -- sus enums de datos (`ScalarOp`, `PredicateOp`, `ContractOp`) no tienen representacion
//! nativa en el bridge (`cxx` solo transporta structs de campos primitivos/`Vec<T>`/`String`,
//! ver `rust/crates/engine-ffi/src/lib.rs`). En vez de inventar una codificacion plana adicional
//! solo para cruzar la frontera, el bridge pasa el JSON canonico `engine.payoff/v1` (ya
//! producido, probado y versionado por `CanonicalVisitor::to_json`/`engine_typed.payoff`) como un
//! simple `String`; este modulo lo compila y evalua enteramente del lado Rust, y unicamente el
//! resultado final de precio (`crate::mc::McEstimate`) cruza de vuelta a C++.

pub mod api;
pub mod api_p;
pub mod basket_api;
pub mod compile;
pub(crate) mod dual;
pub mod eval;
pub mod hedge;
pub mod ir;
mod lrm;
pub mod lsm;
mod sensitivity;

pub use api::{
    hit_probability_gbm_q, payoff_contains_exercise, payoff_exposure_profile_gbm_q, payoff_local_hessian_gbm_q,
    payoff_sensitivity2_gbm_q, payoff_sensitivity_cross_gbm_q, payoff_sensitivity_gbm_q,
    payoff_supports_second_order_lrm, price_payoff_exercise_gbm_q, price_payoff_gbm_q, ExercisePolicyResult,
    LocalHessianEstimate,
};
pub use api_p::{
    forecast_gbm_p, hit_probability_gbm_p, payoff_local_hessian_gbm_p, payoff_sensitivity2_gbm_p,
    payoff_sensitivity_cross_gbm_p, payoff_sensitivity_gbm_p, payoff_supports_second_order_lrm_p,
    pnl_distribution_gbm_p, PnlDistribution,
};
pub use basket_api::price_payoff_basket_gbm_q;
pub use compile::compile;
pub use eval::{evaluate, evaluate_with_events, evaluate_with_events_seeded, EventOutcome, ObservablePath, PathCashflow};
pub use hedge::{
    solve_hedge, solve_hedge_with_constraints, synthesize_hedge_gbm_q, HedgeConstraints, HedgeResidualGreeks,
    HedgeResult,
};
pub use ir::{
    BarrierDirection, BridgePattern, CompiledPayoff, ContractOp, MonitoringMode, PredicateOp, ScalarOp,
    SettlementMode, COMPILED_PAYOFF_VERSION,
};
pub use lsm::ExerciseDateDiagnostic;
