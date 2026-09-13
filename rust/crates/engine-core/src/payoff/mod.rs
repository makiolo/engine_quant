//! Fase 5 (PLAN_PRODUCTS.md §8, §12 "Fase 5 -- `CompiledPayoff` y kernel Rust Q"):
//! representacion plana versionada del AST de payoff (la autoria del AST vive en C++/Python, ver
//! `cpp/engine/include/engine/payoff/`) + interprete pathwise escalar de referencia, ambos en
//! Rust puro, mas los modelos/agregacion Monte Carlo bajo Q que lo consumen (`crate::models::gbm`,
//! `crate::mc`).
//!
//! Alcance (Fase 5 + Fase 6 "barrera discreta vectorizada bajo Q", PLAN_PRODUCTS.md §12):
//! `Zero`/`Cashflow`/`Give`/`Both`/`Scale`/`If`/`When`/`Trigger` (solo `Monitoring::Discrete`),
//! aritmetica escalar completa (`Add`/`Sub`/.../`Clamp`/`EventValue`) y predicados de
//! comparacion/`Between`/`All`/`Any`/`Not`/`EventOccurred`. La resolucion de `Trigger` replica en
//! Rust (`eval::resolve_trigger_states`) la misma semantica de dos pasadas que
//! `ScenarioEvaluator` en C++ (ADR-P0-03/08/10): primer hit, latch, prioridad/orden de `EventId`
//! y "instante activo". `Exercise`/`Average`/`RunningMin`/`RunningMax`/`EventTime`/
//! `DiscountFactor`/`FxConversion`/`Parameter`/`Before`/`After` quedan fuera: requieren ejercicio
//! (Fase 9) o un observable/agregado que ningun modelo Q de esta fase genera, igual que el
//! `ValidationVisitor`/`ScenarioEvaluator` de C++ tampoco los evalua hasta esas fases --
//! `compile::compile` rechaza esos nodos en preflight, nunca ejecutando parcialmente el
//! contrato. `Monitoring::ContinuousApproximation` (Brownian bridge) tambien se rechaza en
//! preflight hasta que exista esa implementacion (ver el doc-comment de `compile::compile`).
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
pub mod compile;
pub mod eval;
pub mod ir;

pub use api::{hit_probability_gbm_q, price_payoff_gbm_q};
pub use compile::compile;
pub use eval::{evaluate, evaluate_with_events, EventOutcome, ObservablePath, PathCashflow};
pub use ir::{
    CompiledPayoff, ContractOp, MonitoringMode, PredicateOp, ScalarOp, SettlementMode, COMPILED_PAYOFF_VERSION,
};
