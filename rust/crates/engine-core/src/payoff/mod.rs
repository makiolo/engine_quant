//! Fase 5 (PLAN_PRODUCTS.md §8, §12 "Fase 5 -- `CompiledPayoff` y kernel Rust Q"):
//! representacion plana versionada del AST de payoff (la autoria del AST vive en C++/Python, ver
//! `cpp/engine/include/engine/payoff/`) + interprete pathwise escalar de referencia, ambos en
//! Rust puro, mas los modelos/agregacion Monte Carlo bajo Q que lo consumen (`crate::models::gbm`,
//! `crate::mc`).
//!
//! Alcance deliberadamente parcial (mismo subconjunto de nodos que el `ScenarioEvaluator` de
//! Fase 1 en C++, sin estado): `Zero`/`Cashflow`/`Give`/`Both`/`Scale`/`If`/`When`, aritmetica
//! escalar completa (`Add`/`Sub`/.../`Clamp`) y predicados de comparacion/`Between`/`All`/`Any`/
//! `Not`. `Trigger`/`Exercise`/`Average`/`RunningMin`/`RunningMax`/`EventTime`/`EventValue`/
//! `DiscountFactor`/`FxConversion`/`Parameter`/`Before`/`After`/`EventOccurred` quedan fuera:
//! todos requieren estado de eventos, ejercicio o observables que ningun modelo Q de esta fase
//! genera (Fase 6+/9), igual que el `ValidationVisitor`/`ScenarioEvaluator` de C++ tampoco los
//! evalua hasta esas fases -- `compile::compile` rechaza esos nodos en preflight con un mensaje
//! que nombra la fase que los introduce, nunca ejecutando parcialmente el contrato.
//!
//! **Como cruza la frontera cxx**: `CompiledPayoff` en si mismo NUNCA se serializa a traves de
//! `cxx` -- sus enums de datos (`ScalarOp`, `PredicateOp`, `ContractOp`) no tienen representacion
//! nativa en el bridge (`cxx` solo transporta structs de campos primitivos/`Vec<T>`/`String`,
//! ver `rust/crates/engine-ffi/src/lib.rs`). En vez de inventar una codificacion plana adicional
//! solo para cruzar la frontera, el bridge pasa el JSON canonico `engine.payoff/v1` (ya
//! producido, probado y versionado por `CanonicalVisitor::to_json`/`engine_typed.payoff`) como un
//! simple `String`; este modulo lo compila y evalua enteramente del lado Rust, y unicamente el
//! resultado final de precio (`crate::mc::McEstimate`) cruza de vuelta a C++.

pub mod compile;
pub mod eval;
pub mod ir;

pub use compile::compile;
pub use eval::{evaluate, ObservablePath, PathCashflow};
pub use ir::{CompiledPayoff, ContractOp, PredicateOp, ScalarOp, COMPILED_PAYOFF_VERSION};
