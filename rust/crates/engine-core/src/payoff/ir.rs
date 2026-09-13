//! `CompiledPayoff`: IR plana versionada (PLAN_PRODUCTS.md §8). Nodos por indice dentro de un
//! `Vec` propio por categoria (`scalar_ops`/`predicate_ops`/`contract_ops`) en vez de punteros
//! compartidos como el AST de autoria en C++ (`std::shared_ptr<const T>`, ADR-P0-07): cada nodo
//! referencia a sus hijos por `usize` dentro del `Vec` de su propia categoria. Los observables se
//! resuelven a slots enteros (`observable_slots`, indexados por `ScalarOp::Fixing`/`Current`) en
//! vez de comparar `String`s en cada evaluacion de ruta (§8 punto 3, "resolucion de nombres a
//! slots enteros").
//!
//! No hay CSE (common-subexpression elimination, §8 punto 4) ni orden topologico explicito mas
//! alla del que ya impone construir el `Vec` en post-orden durante la compilacion (§8 puntos 4-5):
//! ambos son optimizaciones de Fase 11, no requisitos de correccion del interprete de referencia
//! de esta fase.

/// Version del formato `CompiledPayoff`. Sube solo si cambia el significado de un opcode ya
/// publicado (misma regla que `engine_abi_version()` en `cpp/engine/include/engine/abi.h`);
/// anadir un opcode/nodo nuevo no la sube.
pub const COMPILED_PAYOFF_VERSION: u32 = 1;

/// Expresion escalar compilada (PLAN_PRODUCTS.md §3.2). Subconjunto sin estado: ver el
/// doc-comment de `crate::payoff` para los nodos deliberadamente ausentes.
#[derive(Debug, Clone, PartialEq)]
pub enum ScalarOp {
    Constant(f64),
    /// `observable` indexa `CompiledPayoff::observable_slots`; `time` es el instante exacto
    /// pedido en el AST de autoria (sin tolerancia -- la comparacion de tiempos de ADR-P0-01 es
    /// responsabilidad del `ValidationVisitor`/compilador en C++, que ya deduplica/redondea
    /// antes de emitir el JSON canonico).
    Fixing { observable: usize, time: f64 },
    /// Lee el observable en el "instante activo" (cursor de ADR-P0-08) -- ver `eval::eval_scalar`.
    Current { observable: usize },
    Add(usize, usize),
    Sub(usize, usize),
    Mul(usize, usize),
    Div(usize, usize),
    Neg(usize),
    Abs(usize),
    Exp(usize),
    Log(usize),
    Pow(usize, usize),
    Min(usize, usize),
    Max(usize, usize),
    Clamp { value: usize, low: usize, high: usize },
}

/// Predicado compilado (PLAN_PRODUCTS.md §3.3).
#[derive(Debug, Clone, PartialEq)]
pub enum PredicateOp {
    Greater(usize, usize),
    Less(usize, usize),
    GreaterEqual(usize, usize),
    LessEqual(usize, usize),
    Eq { left: usize, right: usize, tolerance: f64 },
    /// Cortocircuito en `eval::eval_predicate` (via `Iterator::all`/`any`), igual que ADR-P0-03
    /// exige para el AST de autoria en C++.
    All(Vec<usize>),
    Any(Vec<usize>),
    Not(usize),
    Between { value: usize, low: usize, high: usize, low_inclusive: bool, high_inclusive: bool },
}

/// Contrato compilado (PLAN_PRODUCTS.md §3.4).
#[derive(Debug, Clone, PartialEq)]
pub enum ContractOp {
    Zero,
    /// La moneda de todo `Cashflow` del programa es `CompiledPayoff::currency` -- Fase 5 no
    /// soporta multi-moneda (ver `compile::compile`, que rechaza un `Cashflow` con una moneda
    /// distinta en preflight).
    Cashflow { amount: usize },
    Give(usize),
    Both(Vec<usize>),
    Scale { factor: usize, child: usize },
    If { condition: usize, if_true: usize, if_false: usize },
    /// Fija el "instante activo" (cursor de ADR-P0-08) para `child`.
    When { time: f64, child: usize },
}

/// IR plana completa de un `PayoffProgram` (PLAN_PRODUCTS.md §8). Inmutable una vez compilada;
/// se interpreta repetidamente, una vez por ruta Monte Carlo, via `crate::payoff::eval::evaluate`.
#[derive(Debug, Clone, PartialEq)]
pub struct CompiledPayoff {
    pub version: u32,
    /// Moneda de reporting unica del programa (ver `ContractOp::Cashflow`).
    pub currency: String,
    /// Nombres canonicos de observable (p.ej. `"EQ.SPOT.AAPL"`), indexados por
    /// `ScalarOp::Fixing`/`Current`. Fase 5 solo simula programas de un unico observable (un
    /// modelo GBM genera un unico subyacente), pero el slot es un `Vec` desde el principio para
    /// no repetir este tipo cuando un modelo multi-activo exista.
    pub observable_slots: Vec<String>,
    pub scalar_ops: Vec<ScalarOp>,
    pub predicate_ops: Vec<PredicateOp>,
    pub contract_ops: Vec<ContractOp>,
    /// Indice en `contract_ops` de la raiz del programa.
    pub root: usize,
}

impl CompiledPayoff {
    /// Todos los instantes en los que un modelo debe generar el (unico) observable para poder
    /// interpretar este programa: los `Fixing` explicitos y los `When` que fijan el "instante
    /// activo" que `Current` puede leer (ADR-P0-08) -- union deduplicada y ordenada ascendente.
    /// Quien simula (p.ej. `models::gbm::Gbm::simulate_at_times`) usa exactamente este conjunto,
    /// nunca una rejilla propia: asi `eval::ObservablePath::value_at` siempre encuentra el
    /// tiempo exacto que le pida el interprete.
    pub fn required_times(&self) -> Vec<f64> {
        let mut times: Vec<f64> = self
            .scalar_ops
            .iter()
            .filter_map(|op| match op {
                ScalarOp::Fixing { time, .. } => Some(*time),
                _ => None,
            })
            .collect();
        times.extend(self.contract_ops.iter().filter_map(|op| match op {
            ContractOp::When { time, .. } => Some(*time),
            _ => None,
        }));
        times.sort_by(|a, b| a.partial_cmp(b).expect("payoff: tiempo no finito en CompiledPayoff"));
        times.dedup_by(|a, b| (*a - *b).abs() < 1e-9);
        times
    }
}
