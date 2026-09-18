//! Sintesis y cobertura de portfolios (PLAN_PRODUCTS.md §11 "Sintesis y cobertura de
//! portfolios"): dado un `portfolio_payoff` objetivo (tipicamente `Both(trades...)`, aunque este
//! modulo no exige esa forma -- cualquier `CompiledPayoff` vale como target) y un universo de
//! instrumentos realmente negociables, resuelve los pesos que minimizan el error de cobertura
//! (`target + sum_i w_i * instrumento_i ~= 0`) sobre una rejilla comun de escenarios Monte Carlo
//! bajo Q, y reporta el riesgo residual -- nunca promete neutralizacion exacta sin comprobar rango
//! (§11, punto 5: "nunca prometer neutralizacion exacta sin comprobar rango").
//!
//! **Por que un solver propio en vez de Eigen/BLAS/LAPACK**: decision tomada explicitamente para
//! esta fase (ver la nota "Estado (avance de esta sesion, para retomar)" al final de la Fase 11 en
//! PLAN_PRODUCTS.md §12) -- el workspace de Rust hoy no depende de ninguna libreria de algebra
//! lineal (`rust/crates/engine-core/Cargo.toml` solo trae `burn`/`serde_json`), y los universos de
//! cobertura previstos son pequenos (unas pocas decenas de instrumentos como mucho): el coste de
//! una dependencia nueva (superficie de build, features, portabilidad CPU/GPU) no se justifica
//! frente a un solver de minimos cuadrados por ecuaciones normales escrito a mano en `f64` puro,
//! del tamano de un modulo, con sus propios tests directos. Mismo criterio que ya aplica
//! `crate::mc` frente a una crate de estadistica (ver su doc-comment) y `calibration::solve_2x2`
//! frente a un solver generico.
//!
//! **Ecuaciones normales + Cholesky, no SVD/QR**: `min_w ||A w - b||^2 + ridge*||w||^2` tiene
//! solucion cerrada `(A^T A + ridge*I) w = A^T b`; `A^T A + ridge*I` es simetrica y, con
//! `ridge > 0`, definida positiva (semidefinida si `ridge = 0` y las columnas de `A` son
//! linealmente dependientes), asi que Cholesky es la factorizacion mas barata que la resuelve --
//! evita el coste y la complejidad de una SVD/QR genericas que este problema (matrices de a lo
//! sumo unas pocas decenas de columnas) no necesita. La contrapartida documentada: las ecuaciones
//! normales elevan al cuadrado el numero de condicion de `A` (`cond(A^T A) = cond(A)^2`), asi que
//! son mas sensibles a colinealidad que una QR -- exactamente el caso de "instrumentos de
//! cobertura redundantes/muy correlacionados" que motiva el parametro `ridge` (regularizacion de
//! Tikhonov): sube el pivote mas pequeno de la matriz normal lo suficiente para que Cholesky exista
//! y el resultado sea numericamente estable, a costa de sesgar los pesos hacia cero. `ridge = 0.0`
//! es valido (sin regularizar) y puede fallar limpiamente si la matriz no es definida positiva --
//! nunca se degrada en silencio a NaN/Inf, ver `cholesky_decompose`.
//!
//! **Que hace esta fase (extendida respecto de la version original de Fase 11)**: implementa los 5
//! pasos de §11 -- rejilla comun de escenarios (paso 3), pesos que minimizan error (paso 4, via
//! ecuaciones normales SIN restricciones o gradiente proyectado CON restricciones de caja, ver
//! `HedgeConstraints`) y riesgo residual (paso 5: `HedgeResult::residuals`/`residual_std`/
//! `residual_max_abs`, mas opcionalmente `residual_greeks` y `gross_notional`). Fuera de alcance
//! deliberado (no oculto, PLAN_PRODUCTS.md §16 y ver el doc-comment de `HedgeConstraints`):
//! riesgo de BASE/correlacion multi-activo (este modulo, igual que `payoff::api`, exige un UNICO
//! observable compartido por el target y todos los instrumentos -- `check_single_observable` --
//! porque el unico modelo disponible, `Gbm`, es de un solo activo; "riesgo de base" solo tiene
//! sentido con varios activos correlacionados, que requeriria un modelo multi-activo nuevo, no
//! una extension de este solver), variables enteras/combinatoria en las restricciones, y
//! restricciones lineales generales mas alla de una caja por instrumento (ver `HedgeConstraints`).
//!
//! **Seed por ruta**: `synthesize_hedge_gbm_q` deriva la seed de cada ruta como
//! `seed.wrapping_add(path_idx as u64)`, mas simple que el `bridge_seed_for_path` (mezcla por
//! constante de Weyl/splitmix64) que usa `payoff::api` -- esa funcion es privada a `api.rs` y no
//! se puede reutilizar desde aqui, pero ademas el proposito es distinto: `api.rs` produce una
//! MEDIDA DE PRECIO de referencia (donde una correlacion espuria entre el RNG de bridge y el
//! indice de ruta seria un sesgo sutil e indeseable), mientras que aqui el bridge (si el target o
//! algun instrumento usa `Monitoring::ContinuousApproximation`) solo necesita ser determinista y
//! con una seed distinta por ruta para que el ajuste de minimos cuadrados sea reproducible -- una
//! aceptacion documentada, no una degradacion oculta.

// Los índices coordinan matrices pequeñas del solver. Las comparaciones negadas con
// floats son deliberadas: además de validar el rango, rechazan NaN de forma explícita.
#![allow(
    clippy::needless_range_loop,
    clippy::neg_cmp_op_on_partial_ord
)]

use crate::backend::{resolve_backend, ComputeBackend, CpuBackend};
use crate::models::gbm::Gbm;
use crate::payoff::api::check_single_observable;
use crate::payoff::compile::compile;
use crate::payoff::eval::{evaluate_with_events_seeded, ObservablePath};
use crate::payoff::ir::CompiledPayoff;
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

fn scalar<B: Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Copia local de `payoff::api::SinglePath` (esa struct es privada a `api.rs`, ver el
/// doc-comment del modulo de por que este archivo no puede importarla): una unica ruta ya
/// simulada, un unico observable (slot 0 -- el mismo preflight `check_single_observable` que usa
/// `payoff::api` garantiza esto antes de llegar aqui, tanto para el target como para cada
/// instrumento).
struct SinglePath<'a> {
    times: &'a [f64],
    values: &'a [f64],
    sigma: f64,
}

impl ObservablePath for SinglePath<'_> {
    fn value_at(&self, _slot: usize, time: f64) -> f64 {
        let idx = self
            .times
            .iter()
            .position(|&t| (t - time).abs() < 1e-9)
            .unwrap_or_else(|| {
                panic!("hedge: tiempo {time} no simulado por el modelo (times={:?})", self.times)
            });
        self.values[idx]
    }

    fn volatility(&self, _slot: usize) -> f64 {
        self.sigma
    }
}

// ---------------------------------------------------------------------------------------------
// Algebra lineal minima: minimos cuadrados regularizados via ecuaciones normales + Cholesky, mas
// gradiente proyectado para restricciones de caja (HedgeConstraints::bounds).
// ---------------------------------------------------------------------------------------------

/// `A^T A`: `n_cols x n_cols`, simetrica por construccion. `design[s]` es la fila del escenario
/// `s` (longitud `n_cols`, ya validada por la llamante).
fn mat_at_a(design: &[Vec<f64>], n_cols: usize) -> Vec<Vec<f64>> {
    let mut result = vec![vec![0.0; n_cols]; n_cols];
    for row in design {
        for i in 0..n_cols {
            if row[i] == 0.0 {
                continue;
            }
            for j in 0..n_cols {
                result[i][j] += row[i] * row[j];
            }
        }
    }
    result
}

/// `A^T b`: vector de longitud `n_cols`.
fn mat_at_b(design: &[Vec<f64>], target: &[f64], n_cols: usize) -> Vec<f64> {
    let mut result = vec![0.0; n_cols];
    for (row, &t) in design.iter().zip(target.iter()) {
        for (i, res) in result.iter_mut().enumerate() {
            *res += row[i] * t;
        }
    }
    result
}

/// Descomposicion de Cholesky `A = L L^T` de una matriz simetrica `a` (`n x n`, se asume
/// simetrica -- solo se lee `a[i][j]` con `j <= i`, nunca se comprueba `a[i][j] == a[j][i]`
/// explicitamente porque ambas llamantes de este modulo construyen `a` como `A^T A + ridge*I`,
/// simetrica por construccion). `Err` explicito -- nunca `NaN`/`Inf` silencioso -- si algun pivote
/// de la diagonal no sale estrictamente positivo: eso significa que `a` no es definida positiva
/// (columnas de `design` linealmente dependientes y `ridge` insuficiente para compensarlo).
/// `pub(crate)` (en vez de privado a este modulo) desde PLAN_IMPROVE_NOTEBOOK.md Fase 3:
/// `crate::models::gbm_basket::GbmBasket` reutiliza esta MISMA factorizacion para aplicar la
/// matriz de correlacion entre activos a shocks normales independientes -- una unica
/// implementacion de Cholesky en el crate, sin duplicarla ni anadir una dependencia de algebra
/// lineal nueva (mismo criterio del doc-comment de este modulo: universos pequenos, `f64` puro).
pub(crate) fn cholesky_decompose(a: &[Vec<f64>]) -> Result<Vec<Vec<f64>>, String> {
    let n = a.len();
    let mut l = vec![vec![0.0; n]; n];
    for j in 0..n {
        let mut sum = a[j][j];
        for k in 0..j {
            sum -= l[j][k] * l[j][k];
        }
        if !(sum > 0.0) {
            return Err(format!(
                "hedge: la descomposicion de Cholesky encontro un pivote no positivo ({sum}) en \
                 la fila {j} -- la matriz normal (A^T A + ridge*I) no es definida positiva \
                 (instrumentos de cobertura redundantes/colineales y ridge insuficiente); suba \
                 `ridge` o elimine instrumentos redundantes del universo de cobertura"
            ));
        }
        l[j][j] = sum.sqrt();
        for i in (j + 1)..n {
            let mut s = a[i][j];
            for k in 0..j {
                s -= l[i][k] * l[j][k];
            }
            l[i][j] = s / l[j][j];
        }
    }
    Ok(l)
}

/// Resuelve `L y = b` (`L` triangular inferior, diagonal ya garantizada > 0 por
/// `cholesky_decompose`).
fn forward_substitution(l: &[Vec<f64>], b: &[f64]) -> Vec<f64> {
    let n = l.len();
    let mut y = vec![0.0; n];
    for i in 0..n {
        let mut sum = b[i];
        for (k, yk) in y.iter().enumerate().take(i) {
            sum -= l[i][k] * yk;
        }
        y[i] = sum / l[i][i];
    }
    y
}

/// Resuelve `L^T x = y` (sustitucion hacia atras; `L^T` nunca se materializa, se lee `L`
/// transpuesta indexando `l[k][i]` en vez de `l[i][k]`).
fn backward_substitution_transpose(l: &[Vec<f64>], y: &[f64]) -> Vec<f64> {
    let n = l.len();
    let mut x = vec![0.0; n];
    for i in (0..n).rev() {
        let mut sum = y[i];
        for k in (i + 1)..n {
            sum -= l[k][i] * x[k];
        }
        x[i] = sum / l[i][i];
    }
    x
}

fn validate_design_target_shapes(design: &[Vec<f64>], target: &[f64]) -> Result<usize, String> {
    if design.is_empty() || target.is_empty() {
        return Err("hedge: design/target no pueden estar vacios (se requiere al menos un escenario)".to_string());
    }
    if design.len() != target.len() {
        return Err(format!(
            "hedge: design tiene {} filas (escenarios) pero target tiene {} elementos -- deben coincidir",
            design.len(),
            target.len()
        ));
    }
    let n_cols = design[0].len();
    if n_cols == 0 {
        return Err("hedge: design no puede tener filas de longitud 0 (ningun instrumento en el universo)".to_string());
    }
    for (idx, row) in design.iter().enumerate() {
        if row.len() != n_cols {
            return Err(format!(
                "hedge: la fila {idx} de design tiene longitud {} pero se esperaba {n_cols} \
                 (todas las filas deben tener la misma longitud, una entrada por instrumento)",
                row.len()
            ));
        }
    }
    Ok(n_cols)
}

/// `min_w ||A w - b||^2 + ridge*||w||^2` via ecuaciones normales `(A^T A + ridge*I) w = A^T b`,
/// resueltas por Cholesky (ver el doc-comment del modulo para el porque de esta eleccion frente a
/// SVD/QR/una dependencia externa). `design[s]` es la fila del escenario `s` (longitud = numero de
/// instrumentos, igual para todas las filas); `target[s]` el valor a ajustar en ese escenario.
///
/// `Err` (nunca `NaN`/degradacion silenciosa) si: `design`/`target` estan vacios, sus longitudes
/// no coinciden, alguna fila de `design` tiene longitud distinta de las demas, `design` tiene
/// columnas de longitud 0, `ridge < 0`, o Cholesky encuentra un pivote no positivo.
fn solve_least_squares_normal_equations(design: &[Vec<f64>], target: &[f64], ridge: f64) -> Result<Vec<f64>, String> {
    let n_cols = validate_design_target_shapes(design, target)?;
    if !(ridge >= 0.0) {
        return Err(format!("hedge: ridge debe ser >= 0 (recibido {ridge})"));
    }

    let mut ata = mat_at_a(design, n_cols);
    for (i, row) in ata.iter_mut().enumerate() {
        row[i] += ridge;
    }
    let atb = mat_at_b(design, target, n_cols);

    let l = cholesky_decompose(&ata)?;
    let y = forward_substitution(&l, &atb);
    Ok(backward_substitution_transpose(&l, &y))
}

/// Gradiente proyectado (paso fijo, cota de Lipschitz de Frobenius) para
/// `min_w ||A w - b||^2 + ridge*||w||^2` sujeto a `lower[i] <= w[i] <= upper[i]`
/// (PLAN_PRODUCTS.md §12 Fase 11, item pendiente "restricciones de tipo LP/QP (posiciones
/// minimas/maximas)"). Deliberadamente NO un solver QP generico (simplex/active-set/
/// interior-point): el objetivo es convexo (fuertemente convexo si `ridge > 0`) y la proyeccion
/// sobre una caja es un simple `clamp` por coordenada, asi que gradiente proyectado con paso fijo
/// converge de sobra para los universos de cobertura pequenos (unas pocas decenas de instrumentos)
/// que motivan este modulo (mismo criterio que ya justifico Cholesky sobre SVD/QR para el caso sin
/// restricciones, ver el doc-comment del modulo). `lower[i] > upper[i]` es un error de la llamante,
/// ya validado antes de llegar aqui (`HedgeConstraints::bounds`).
fn solve_least_squares_box_constrained(
    design: &[Vec<f64>],
    target: &[f64],
    ridge: f64,
    lower: &[f64],
    upper: &[f64],
) -> Result<Vec<f64>, String> {
    let n_cols = validate_design_target_shapes(design, target)?;
    if !(ridge >= 0.0) {
        return Err(format!("hedge: ridge debe ser >= 0 (recibido {ridge})"));
    }
    if lower.len() != n_cols || upper.len() != n_cols {
        return Err(format!(
            "hedge: lower/upper deben tener longitud {n_cols} (uno por instrumento), recibido {}/{}",
            lower.len(),
            upper.len()
        ));
    }

    let clamp = |w: &mut [f64]| {
        for i in 0..n_cols {
            if w[i] < lower[i] {
                w[i] = lower[i];
            }
            if w[i] > upper[i] {
                w[i] = upper[i];
            }
        }
    };

    // Punto de partida: la solucion sin restricciones si Cholesky tiene exito (mejor warm start),
    // si no el vector cero -- en ambos casos recortado a la caja antes de iterar.
    let mut w = solve_least_squares_normal_equations(design, target, ridge).unwrap_or_else(|_| vec![0.0; n_cols]);
    clamp(&mut w);

    // Cota de Lipschitz del gradiente de f(w) = ||Aw-b||^2 + ridge||w||^2: grad = 2*A^T(Aw-b) +
    // 2*ridge*w, hessiano = 2*(A^T A + ridge*I); la norma de Frobenius de A acota su norma
    // espectral (||A^T A||_2 <= ||A||_F^2), cota conservadora pero segura que evita una iteracion
    // de potencias para el autovalor mayor exacto.
    let frobenius_sq: f64 = design.iter().flat_map(|row| row.iter()).map(|x| x * x).sum();
    let lipschitz = 2.0 * (frobenius_sq + ridge);
    if lipschitz <= 0.0 {
        // A es toda ceros y ridge=0: el termino ||Aw-b||^2 es constante (= ||target||^2) para
        // cualquier w, asi que el punto de partida (ya recortado a la caja) ya minimiza.
        return Ok(w);
    }
    let step = 1.0 / lipschitz;

    const MAX_ITERS: usize = 2_000;
    const TOL: f64 = 1e-12;
    for _ in 0..MAX_ITERS {
        let residual: Vec<f64> = design
            .iter()
            .map(|row| row.iter().zip(w.iter()).map(|(a, wi)| a * wi).sum::<f64>())
            .zip(target.iter())
            .map(|(aw, t)| aw - t)
            .collect();

        let mut grad = vec![0.0; n_cols];
        for (row, r) in design.iter().zip(residual.iter()) {
            for (g, a) in grad.iter_mut().zip(row.iter()) {
                *g += a * r;
            }
        }
        for i in 0..n_cols {
            grad[i] = 2.0 * (grad[i] + ridge * w[i]);
        }

        let mut max_delta: f64 = 0.0;
        for i in 0..n_cols {
            let mut next = w[i] - step * grad[i];
            if next < lower[i] {
                next = lower[i];
            }
            if next > upper[i] {
                next = upper[i];
            }
            max_delta = max_delta.max((next - w[i]).abs());
            w[i] = next;
        }
        if max_delta < TOL {
            break;
        }
    }

    Ok(w)
}

// ---------------------------------------------------------------------------------------------
// API de resultado y orquestacion sobre escenarios ya evaluados (§11, pasos 4-5).
// ---------------------------------------------------------------------------------------------

/// Restricciones opcionales sobre `weights` que `solve_hedge_with_constraints`/
/// `synthesize_hedge_gbm_q` aceptan (PLAN_PRODUCTS.md §12 Fase 11, items pendientes "liquidez" y
/// "restricciones de tipo LP/QP (posiciones minimas/maximas...)"). Alcance deliberadamente
/// LIMITADO (documentado, no oculto, ver tambien el doc-comment del modulo):
///
/// - `bounds`: restricciones de CAJA por instrumento (`lower_i <= weights[i] <= upper_i`), no
///   restricciones lineales generales ni variables enteras -- §11 solo pide "posiciones
///   minimas/maximas", no combinatoria;
/// - `max_gross_notional`: limite de liquidez sobre `sum(|weights[i]| * instrument_prices[i])`
///   (requiere `instrument_prices`), aplicado por un escalado UNIFORME de `weights` tras resolver
///   -- no una reoptimizacion sujeta al limite. Si ese escalado violase algun `bounds`,
///   `solve_hedge_with_constraints` devuelve `Err` en vez de romper una restriccion en silencio
///   (conflicto de restricciones genuino: liquidez y posiciones minimas/maximas son incompatibles
///   en ese punto).
#[derive(Debug, Clone, Default)]
pub struct HedgeConstraints {
    pub bounds: Option<Vec<(f64, f64)>>,
    pub max_gross_notional: Option<f64>,
}

/// Sensibilidades residuales (bump-and-reval, numeros aleatorios comunes) de la cartera cubierta
/// (`target + sum_i weights[i] * instrumento_i`, con `weights` YA RESUELTOS -- no se reoptimizan
/// bajo el parametro bumpeado) respecto de los 4 parametros de `Gbm` (PLAN_PRODUCTS.md §12 Fase
/// 11, item pendiente "Greeks residuales" del solver de hedge). Deliberadamente NO usa el metodo
/// pathwise `Dual` de `super::sensitivity` (que diferencia UN `CompiledPayoff` a la vez): aqui la
/// cantidad a diferenciar es una combinacion lineal de varios payoffs con pesos fijos, y
/// expresarla como una unica pasada `Dual` exigiria evaluar cada instrumento por separado de
/// todas formas -- bump-and-reval sobre el PORTFOLIO agregado (una unica cantidad escalar por
/// escenario bumpeado) es mas simple y reutiliza `evaluate_hedge_scenario` tal cual, con el mismo
/// `seed` en ambas evaluaciones bumpeadas (mismo criterio que
/// `payoff::api::payoff_sensitivity_bump_and_reval`).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HedgeResidualGreeks {
    pub delta: f64,
    pub rho: f64,
    pub dividend_yield: f64,
    pub vega: f64,
}

/// Resultado de resolver una cobertura: pesos por instrumento mas el riesgo residual reportado
/// escenario a escenario (§11, paso 5: "devolver hedge y riesgo residual").
#[derive(Debug, Clone, PartialEq)]
pub struct HedgeResult {
    pub weights: Vec<f64>,
    /// Uno por escenario: `target[s] + sum_i(weights[i] * design[s][i])` -- el error terminal de
    /// cobertura que pide reportar §11 ("error terminal por escenario"). Cero exacto significaria
    /// neutralizacion perfecta en ese escenario; nunca se asume, solo se reporta (§11, punto 5).
    pub residuals: Vec<f64>,
    pub residual_mean: f64,
    /// Desviacion estandar MUESTRAL (`n-1`) de `residuals`; `0.0` si solo hay un escenario (mismo
    /// criterio que `crate::mc::aggregate` para `n=1`).
    pub residual_std: f64,
    pub residual_max_abs: f64,
    /// `Some(sum(|weights[i]| * instrument_prices[i]))` si se proporcionaron precios de los
    /// instrumentos (§11: "coste/prima"); `None` si no se proporcionaron -- nunca se asume un
    /// coste de cero por omision, se reporta la ausencia del dato.
    pub cost: Option<f64>,
    /// `Some(sum(|weights[i]| * instrument_prices[i]))` -- identico calculo que `cost` (mismo
    /// dato, dos nombres: `cost` sopesa el signo economico de la cobertura, `gross_notional` es
    /// la envolvente L1 que `HedgeConstraints::max_gross_notional` restringe). `None` si no se
    /// proporcionaron `instrument_prices`.
    pub gross_notional: Option<f64>,
    /// `Some(...)` solo si se pidio `compute_residual_greeks=true` en `synthesize_hedge_gbm_q`;
    /// `None` si no se pidio -- nunca un cero por omision.
    pub residual_greeks: Option<HedgeResidualGreeks>,
}

/// Resuelve `weights` tales que `target[s] + sum_i(weights[i] * design[s][i]) ~= 0` para todo
/// escenario `s` (la cartera de instrumentos NETEA el target) por minimos cuadrados regularizados,
/// con las restricciones opcionales de `constraints` (PLAN_PRODUCTS.md §12 Fase 11): `A = design`,
/// se resuelve `A w ~= -target` (minimizar `||target + A w||^2` es identico a minimizar
/// `||A w - (-target)||^2`, de ahi el signo).
///
/// Valida formas ANTES de llamar al solver: todas las filas de `design` deben tener la misma
/// longitud, que ademas debe coincidir con `instrument_prices.len()` y con
/// `constraints.bounds.len()` si se proporcionan. `constraints.max_gross_notional` sin
/// `instrument_prices` es un error explicito (no se puede medir notional bruto sin precios).
pub fn solve_hedge_with_constraints(
    design: &[Vec<f64>],
    target: &[f64],
    instrument_prices: Option<&[f64]>,
    ridge: f64,
    constraints: &HedgeConstraints,
) -> Result<HedgeResult, String> {
    let n_instruments = validate_design_target_shapes(design, target)?;
    if let Some(prices) = instrument_prices {
        if prices.len() != n_instruments {
            return Err(format!(
                "hedge: instrument_prices tiene {} elementos pero design tiene {n_instruments} \
                 instrumentos -- deben coincidir",
                prices.len()
            ));
        }
    }
    if let Some(bounds) = &constraints.bounds {
        if bounds.len() != n_instruments {
            return Err(format!(
                "hedge: constraints.bounds tiene {} elementos pero design tiene {n_instruments} \
                 instrumentos -- deben coincidir",
                bounds.len()
            ));
        }
        for (idx, (lo, hi)) in bounds.iter().enumerate() {
            if !(lo <= hi) {
                return Err(format!(
                    "hedge: constraints.bounds[{idx}] invalido: lower ({lo}) debe ser <= upper ({hi})"
                ));
            }
        }
    }
    if let Some(max_gn) = constraints.max_gross_notional {
        if instrument_prices.is_none() {
            return Err(
                "hedge: constraints.max_gross_notional requiere instrument_prices (no se puede \
                 medir notional bruto sin precios)"
                    .to_string(),
            );
        }
        if !(max_gn >= 0.0) {
            return Err(format!("hedge: constraints.max_gross_notional debe ser >= 0 (recibido {max_gn})"));
        }
    }

    let neg_target: Vec<f64> = target.iter().map(|t| -t).collect();
    let mut weights = match &constraints.bounds {
        None => solve_least_squares_normal_equations(design, &neg_target, ridge)?,
        Some(bounds) => {
            let lower: Vec<f64> = bounds.iter().map(|(lo, _)| *lo).collect();
            let upper: Vec<f64> = bounds.iter().map(|(_, hi)| *hi).collect();
            solve_least_squares_box_constrained(design, &neg_target, ridge, &lower, &upper)?
        }
    };

    let mut gross_notional = instrument_prices
        .map(|prices| weights.iter().zip(prices.iter()).map(|(w, p)| w.abs() * p).sum::<f64>());

    if let (Some(max_gn), Some(prices)) = (constraints.max_gross_notional, instrument_prices) {
        let current = gross_notional.expect("gross_notional ya calculado arriba cuando hay instrument_prices");
        if current > max_gn && current > 0.0 {
            let scale = max_gn / current;
            for w in &mut weights {
                *w *= scale;
            }
            if let Some(bounds) = &constraints.bounds {
                for (idx, (lo, hi)) in bounds.iter().enumerate() {
                    if weights[idx] < *lo - 1e-9 || weights[idx] > *hi + 1e-9 {
                        return Err(format!(
                            "hedge: el escalado de liquidez (factor {scale}) para respetar \
                             max_gross_notional ({max_gn}) rompe constraints.bounds[{idx}] \
                             ([{lo}, {hi}], resultado {}) -- restricciones incompatibles en este \
                             punto",
                            weights[idx]
                        ));
                    }
                }
            }
            gross_notional = Some(weights.iter().zip(prices.iter()).map(|(w, p)| w.abs() * p).sum());
        }
    }

    let residuals: Vec<f64> = design
        .iter()
        .zip(target.iter())
        .map(|(row, &t)| t + row.iter().zip(weights.iter()).map(|(a, w)| a * w).sum::<f64>())
        .collect();

    let n = residuals.len() as f64;
    let residual_mean = residuals.iter().sum::<f64>() / n;
    let residual_std = if residuals.len() > 1 {
        (residuals.iter().map(|r| (r - residual_mean).powi(2)).sum::<f64>() / (n - 1.0)).sqrt()
    } else {
        0.0
    };
    let residual_max_abs = residuals.iter().fold(0.0_f64, |acc, r| acc.max(r.abs()));
    let cost = gross_notional;

    Ok(HedgeResult {
        weights,
        residuals,
        residual_mean,
        residual_std,
        residual_max_abs,
        cost,
        gross_notional,
        residual_greeks: None,
    })
}

/// Igual que `solve_hedge_with_constraints` pero sin restricciones (`HedgeConstraints::default()`)
/// -- firma preservada tal cual para no romper a las llamantes existentes (esta funcion ya era
/// publica antes de que Fase 11 anadiera `bounds`/`max_gross_notional`).
pub fn solve_hedge(
    design: &[Vec<f64>],
    target: &[f64],
    instrument_prices: Option<&[f64]>,
    ridge: f64,
) -> Result<HedgeResult, String> {
    solve_hedge_with_constraints(design, target, instrument_prices, ridge, &HedgeConstraints::default())
}

// ---------------------------------------------------------------------------------------------
// Sintesis de cobertura bajo GBM sobre una rejilla comun de escenarios (§11, pasos 1-3 + 4 via
// solve_hedge_with_constraints, mas Greeks residuales opcionales).
// ---------------------------------------------------------------------------------------------

/// Copia local de `payoff::api::simulate_gbm_columns_at` (privada a `api.rs`, ver el doc-comment
/// del modulo): simula bajo GBM el unico observable en `times` (ya ordenado/deduplicado/> 0) y
/// devuelve `columns[i][path]` = valor de ese observable en `times[i]` para la ruta `path`.
/// Generica sobre `B` -- mismo patron que el resto de `payoff::api`.
#[allow(clippy::too_many_arguments)]
fn simulate_gbm_columns_at<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    times: &[f64],
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Vec<Vec<f64>> {
    B::seed(device, seed);
    let model = Gbm::<B>::new(scalar(s0, device), scalar(r, device), scalar(q, device), scalar(sigma, device));
    let simulated = model.simulate_at_times(times, n_paths as usize, device);
    simulated.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect()
}

/// Rejilla comun de escenarios ya evaluada (§11, pasos 3-4): un valor presente por ruta del
/// target y de cada instrumento, sobre las MISMAS rutas simuladas -- extraido como funcion
/// reutilizable porque tanto `synthesize_hedge_gbm_q` (resolver los pesos) como
/// `hedge_residual_greeks_on` (bump-and-reval de las sensibilidades de la cobertura ya resuelta)
/// necesitan exactamente este mismo calculo, solo que con `s0`/`r`/`q`/`sigma` distintos.
struct HedgeScenario {
    target_pv: Vec<f64>,
    /// `design[path][instrument]`.
    design: Vec<Vec<f64>>,
}

#[allow(clippy::too_many_arguments)]
fn evaluate_hedge_scenario<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    target_payoff: &CompiledPayoff,
    instrument_payoffs: &[CompiledPayoff],
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<HedgeScenario, String> {
    let mut simulation_times: Vec<f64> = target_payoff.required_times();
    for instrument_payoff in instrument_payoffs {
        simulation_times.extend(instrument_payoff.required_times());
    }
    simulation_times.sort_by(|a, b| a.partial_cmp(b).expect("hedge: tiempo no finito"));
    simulation_times.dedup_by(|a, b| (*a - *b).abs() < 1e-9);
    if simulation_times.is_empty() {
        return Err(
            "hedge: ni el target ni los instrumentos dependen de ningun instante de mercado (nada que simular bajo Q)"
                .to_string(),
        );
    }

    let n_paths_usize = n_paths as usize;
    let columns = simulate_gbm_columns_at::<B>(device, &simulation_times, s0, r, q, sigma, n_paths, seed);

    let mut target_pv = Vec::with_capacity(n_paths_usize);
    let mut design: Vec<Vec<f64>> = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &simulation_times, values: &values, sigma };
        let path_seed = seed.wrapping_add(path_idx as u64);

        let (target_ledger, _events) = evaluate_with_events_seeded(target_payoff, &path, path_seed);
        let target_present_value: f64 =
            target_ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum();
        target_pv.push(target_present_value);

        let mut row = Vec::with_capacity(instrument_payoffs.len());
        for instrument_payoff in instrument_payoffs {
            let (ledger, _events) = evaluate_with_events_seeded(instrument_payoff, &path, path_seed);
            let pv: f64 = ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum();
            row.push(pv);
        }
        design.push(row);
    }

    Ok(HedgeScenario { target_pv, design })
}

/// Media sobre rutas de `target_pv[path] + sum_i(weights[i] * design[path][i])` -- el precio
/// agregado de la cartera cubierta con los pesos YA RESUELTOS, usado como la cantidad escalar que
/// `hedge_residual_greeks_on` diferencia por bump-and-reval.
fn portfolio_mean_pv(scenario: &HedgeScenario, weights: &[f64]) -> f64 {
    let n_paths = scenario.target_pv.len() as f64;
    let target_mean = scenario.target_pv.iter().sum::<f64>() / n_paths;
    let mut instrument_means = vec![0.0; weights.len()];
    for row in &scenario.design {
        for (m, v) in instrument_means.iter_mut().zip(row.iter()) {
            *m += v;
        }
    }
    for m in &mut instrument_means {
        *m /= n_paths;
    }
    target_mean + weights.iter().zip(instrument_means.iter()).map(|(w, m)| w * m).sum::<f64>()
}

/// Greeks residuales de la cartera cubierta (bump-and-reval, numeros aleatorios comunes -- ver el
/// doc-comment de `HedgeResidualGreeks`). Los `weights` NO se reoptimizan bajo el parametro
/// bumpeado: es, por definicion, el riesgo residual de LA COBERTURA YA PUESTA.
#[allow(clippy::too_many_arguments)]
fn hedge_residual_greeks_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    target_payoff: &CompiledPayoff,
    instrument_payoffs: &[CompiledPayoff],
    weights: &[f64],
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<HedgeResidualGreeks, String> {
    // Misma convencion de bump que `payoff::api::payoff_sensitivity_bump_and_reval`: 1% relativo
    // con un piso absoluto, para que la senal (diferencia de precio real) domine sobre el ruido
    // de discretizacion Monte Carlo entre las dos evaluaciones bumpeadas.
    const RELATIVE_BUMP: f64 = 1e-2;
    const MIN_ABSOLUTE_BUMP: f64 = 1e-4;
    let bump = |v: f64| (RELATIVE_BUMP * v.abs()).max(MIN_ABSOLUTE_BUMP);

    let portfolio_pv_at = |s0: f64, r: f64, q: f64, sigma: f64| -> Result<f64, String> {
        let scenario =
            evaluate_hedge_scenario::<B>(device, target_payoff, instrument_payoffs, s0, r, q, sigma, n_paths, seed)?;
        Ok(portfolio_mean_pv(&scenario, weights))
    };

    let h_s0 = bump(s0);
    let delta = (portfolio_pv_at(s0 + h_s0, r, q, sigma)? - portfolio_pv_at(s0 - h_s0, r, q, sigma)?) / (2.0 * h_s0);
    let h_r = bump(r);
    let rho = (portfolio_pv_at(s0, r + h_r, q, sigma)? - portfolio_pv_at(s0, r - h_r, q, sigma)?) / (2.0 * h_r);
    let h_q = bump(q);
    let dividend_yield =
        (portfolio_pv_at(s0, r, q + h_q, sigma)? - portfolio_pv_at(s0, r, q - h_q, sigma)?) / (2.0 * h_q);
    let h_sigma = bump(sigma);
    let vega = (portfolio_pv_at(s0, r, q, sigma + h_sigma)? - portfolio_pv_at(s0, r, q, sigma - h_sigma)?)
        / (2.0 * h_sigma);

    Ok(HedgeResidualGreeks { delta, rho, dividend_yield, vega })
}

/// Sintetiza una cobertura bajo GBM/Q para `target_spec_json` con el universo de instrumentos
/// `instrument_specs_json` (§11, pasos 1-5 completos, mas restricciones/Greeks opcionales de
/// Fase 11 extendida):
///
/// 1. `Err` si `instrument_specs_json` esta vacio (ningun instrumento con el que cubrir) o si
///    `n_paths == 0`.
/// 2. compila el target y cada instrumento (`payoff::compile::compile`);
/// 3. preflight de observable unico (`payoff::api::check_single_observable`) sobre el target Y
///    cada instrumento -- ninguno puede referenciar un observable que este `Gbm` no genera;
/// 4. rejilla de tiempos compartida y valor presente de cada ruta/contrato via
///    `evaluate_hedge_scenario`;
/// 5. `solve_hedge_with_constraints(&scenario.design, &scenario.target_pv, instrument_prices,
///    ridge, constraints)`;
/// 6. si `compute_residual_greeks`, adjunta `HedgeResidualGreeks` (bump-and-reval sobre los pesos
///    ya resueltos, ver `hedge_residual_greeks_on`).
#[allow(clippy::too_many_arguments)]
pub fn synthesize_hedge_gbm_q(
    backend: &str,
    target_spec_json: &str,
    instrument_specs_json: &[String],
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    instrument_prices: Option<&[f64]>,
    ridge: f64,
    constraints: &HedgeConstraints,
    compute_residual_greeks: bool,
    n_paths: u64,
    seed: u64,
) -> Result<HedgeResult, String> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            synthesize_hedge_gbm_q_on::<CpuBackend>(
                &device,
                target_spec_json,
                instrument_specs_json,
                observable,
                s0,
                r,
                q,
                sigma,
                instrument_prices,
                ridge,
                constraints,
                compute_residual_greeks,
                n_paths,
                seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                synthesize_hedge_gbm_q_on::<crate::backend::GpuBackend>(
                    &device,
                    target_spec_json,
                    instrument_specs_json,
                    observable,
                    s0,
                    r,
                    q,
                    sigma,
                    instrument_prices,
                    ridge,
                    constraints,
                    compute_residual_greeks,
                    n_paths,
                    seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                // Inalcanzable en la practica: `resolve_backend` ya cae a `Cpu` cuando este build
                // no tiene la feature `gpu` (ver el mismo comentario en `payoff::api`).
                let device = burn::tensor::Device::<CpuBackend>::default();
                synthesize_hedge_gbm_q_on::<CpuBackend>(
                    &device,
                    target_spec_json,
                    instrument_specs_json,
                    observable,
                    s0,
                    r,
                    q,
                    sigma,
                    instrument_prices,
                    ridge,
                    constraints,
                    compute_residual_greeks,
                    n_paths,
                    seed,
                )
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn synthesize_hedge_gbm_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    target_spec_json: &str,
    instrument_specs_json: &[String],
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    instrument_prices: Option<&[f64]>,
    ridge: f64,
    constraints: &HedgeConstraints,
    compute_residual_greeks: bool,
    n_paths: u64,
    seed: u64,
) -> Result<HedgeResult, String> {
    if instrument_specs_json.is_empty() {
        return Err("hedge: instrument_specs_json no puede estar vacio (universo de cobertura vacio)".to_string());
    }
    if n_paths == 0 {
        return Err("hedge: n_paths debe ser > 0".to_string());
    }

    let target_payoff = compile(target_spec_json)?;
    check_single_observable(&target_payoff, observable, n_paths)?;

    let instrument_payoffs: Vec<CompiledPayoff> =
        instrument_specs_json.iter().map(|spec| compile(spec)).collect::<Result<Vec<_>, _>>()?;
    for instrument_payoff in &instrument_payoffs {
        check_single_observable(instrument_payoff, observable, n_paths)?;
    }

    let scenario =
        evaluate_hedge_scenario::<B>(device, &target_payoff, &instrument_payoffs, s0, r, q, sigma, n_paths, seed)?;

    let mut result =
        solve_hedge_with_constraints(&scenario.design, &scenario.target_pv, instrument_prices, ridge, constraints)?;

    if compute_residual_greeks {
        result.residual_greeks = Some(hedge_residual_greeks_on::<B>(
            device,
            &target_payoff,
            &instrument_payoffs,
            &result.weights,
            s0,
            r,
            q,
            sigma,
            n_paths,
            seed,
        )?);
    }

    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    // -----------------------------------------------------------------------------------------
    // solve_least_squares_normal_equations: tests directos sobre el algebra lineal.
    // -----------------------------------------------------------------------------------------

    #[test]
    fn two_by_two_system_matches_hand_computed_solution() {
        // A = [[2,1],[1,3]], b=[8,13]. A es cuadrada e invertible: min ||Aw-b||^2 = 0 en
        // w = A^{-1} b, asi que la solucion de minimos cuadrados (con ridge=0) debe reproducir
        // esa solucion exacta.
        // A mano: 2*w1+w2=8; w1+3*w2=13 => w2=8-2*w1 => w1+3*(8-2*w1)=13 => -5*w1=-11 => w1=2.2,
        // w2=3.6.
        let design = vec![vec![2.0, 1.0], vec![1.0, 3.0]];
        let target = vec![8.0, 13.0];
        let w = solve_least_squares_normal_equations(&design, &target, 0.0).unwrap();
        assert!((w[0] - 2.2).abs() < 1e-9, "w={w:?}");
        assert!((w[1] - 3.6).abs() < 1e-9, "w={w:?}");
    }

    #[test]
    fn exact_linear_combination_is_recovered_without_noise() {
        // target construido EXACTAMENTE como 2*col0 - 3*col1, sin ruido: un sistema
        // sobredeterminado (5 escenarios, 2 instrumentos) pero consistente debe recuperar los
        // pesos exactos.
        let design = vec![
            vec![1.0, 0.5],
            vec![2.0, 1.0],
            vec![-1.0, 3.0],
            vec![4.0, -2.0],
            vec![0.5, 0.25],
        ];
        let known_weights = [2.0, -3.0];
        let target: Vec<f64> =
            design.iter().map(|row| known_weights[0] * row[0] + known_weights[1] * row[1]).collect();

        let w = solve_least_squares_normal_equations(&design, &target, 0.0).unwrap();
        assert!((w[0] - known_weights[0]).abs() < 1e-9, "w={w:?}");
        assert!((w[1] - known_weights[1]).abs() < 1e-9, "w={w:?}");
    }

    #[test]
    fn duplicated_columns_are_rank_deficient_but_ridge_fixes_it() {
        // Dos columnas identicas: A^T A tiene rango 1 (matriz [[s,s],[s,s]]), asi que el segundo
        // pivote de Cholesky sale <= 0 (exactamente 0 en aritmetica exacta) -- documentamos que
        // con ridge=0.0 esto puede fallar (aceptamos tambien un resultado que salga "por
        // casualidad" de un redondeo de punto flotante, pero comprobamos que si sale Ok los pesos
        // sean finitos). Con ridge > 0.0 el pivote sube estrictamente por encima de cero y el
        // solve SIEMPRE tiene que tener exito con pesos finitos.
        let design = vec![vec![1.0, 1.0], vec![2.0, 2.0], vec![3.0, 3.0], vec![-1.0, -1.0]];
        let target = vec![1.0, 2.0, 3.0, -1.0];

        match solve_least_squares_normal_equations(&design, &target, 0.0) {
            Err(msg) => assert!(msg.contains("pivote"), "mensaje inesperado: {msg}"),
            Ok(w) => assert!(w.iter().all(|x| x.is_finite()), "w degenerado no finito: {w:?}"),
        }

        let w_ridge = solve_least_squares_normal_equations(&design, &target, 0.01)
            .expect("con ridge > 0 el solve debe tener exito pese a la colinealidad");
        assert!(w_ridge.iter().all(|x| x.is_finite()), "w_ridge={w_ridge:?}");
    }

    #[test]
    fn rejects_empty_design_or_target() {
        let err = solve_least_squares_normal_equations(&[], &[], 0.0).unwrap_err();
        assert!(err.contains("vacio") || err.contains("vacios"));
    }

    #[test]
    fn rejects_inconsistent_row_lengths() {
        let design = vec![vec![1.0, 2.0], vec![1.0]];
        let target = vec![1.0, 2.0];
        let err = solve_least_squares_normal_equations(&design, &target, 0.0).unwrap_err();
        assert!(err.contains("longitud"), "mensaje inesperado: {err}");
    }

    #[test]
    fn rejects_negative_ridge() {
        let design = vec![vec![1.0]];
        let target = vec![1.0];
        let err = solve_least_squares_normal_equations(&design, &target, -0.1).unwrap_err();
        assert!(err.contains("ridge"), "mensaje inesperado: {err}");
    }

    // -----------------------------------------------------------------------------------------
    // solve_least_squares_box_constrained: gradiente proyectado.
    // -----------------------------------------------------------------------------------------

    #[test]
    fn box_constrained_matches_unconstrained_when_bounds_are_slack() {
        let design = vec![vec![2.0, 1.0], vec![1.0, 3.0]];
        let target = vec![8.0, 13.0];
        let unconstrained = solve_least_squares_normal_equations(&design, &target, 0.0).unwrap();
        let constrained =
            solve_least_squares_box_constrained(&design, &target, 0.0, &[-100.0, -100.0], &[100.0, 100.0]).unwrap();
        for (u, c) in unconstrained.iter().zip(constrained.iter()) {
            assert!((u - c).abs() < 1e-4, "unconstrained={unconstrained:?} constrained={constrained:?}");
        }
    }

    #[test]
    fn box_constrained_clamps_to_upper_bound_when_optimum_exceeds_it() {
        // Un unico instrumento perfectamente correlacionado con el target (columna = target):
        // el optimo sin restricciones es w=1.0 exactamente. Forzar upper=0.3 debe pegar el
        // resultado a ese borde.
        let target = vec![1.0, 2.0, 3.0, 4.0];
        let design: Vec<Vec<f64>> = target.iter().map(|&t| vec![t]).collect();
        let w = solve_least_squares_box_constrained(&design, &target, 0.0, &[0.0], &[0.3]).unwrap();
        assert!((w[0] - 0.3).abs() < 1e-6, "w={w:?}");
    }

    #[test]
    fn box_constrained_respects_an_equality_bound() {
        // lower == upper: la unica solucion factible es ese valor exacto, sea cual sea el target.
        let design = vec![vec![1.0, 0.0], vec![0.0, 1.0], vec![1.0, 1.0]];
        let target = vec![5.0, 5.0, 5.0];
        let w = solve_least_squares_box_constrained(&design, &target, 0.001, &[2.0, -100.0], &[2.0, 100.0]).unwrap();
        assert!((w[0] - 2.0).abs() < 1e-6, "w={w:?}");
    }

    #[test]
    fn box_constrained_rejects_mismatched_bounds_length() {
        let design = vec![vec![1.0, 2.0], vec![3.0, 4.0]];
        let target = vec![1.0, 2.0];
        let err = solve_least_squares_box_constrained(&design, &target, 0.0, &[0.0], &[1.0]).unwrap_err();
        assert!(err.contains("lower/upper"), "mensaje inesperado: {err}");
    }

    // -----------------------------------------------------------------------------------------
    // solve_hedge / solve_hedge_with_constraints: semantica de signo y validacion de formas.
    // -----------------------------------------------------------------------------------------

    #[test]
    fn hedging_an_instrument_with_itself_gives_weight_minus_one_and_zero_residual() {
        // El unico instrumento de cobertura es EL MISMO target: la cobertura obvia es una
        // posicion corta de -1 unidades, que neutraliza exactamente en cada escenario.
        let pv_per_scenario = vec![5.0, -2.0, 10.0, 0.5, -7.0];
        let design: Vec<Vec<f64>> = pv_per_scenario.iter().map(|&v| vec![v]).collect();

        let result = solve_hedge(&design, &pv_per_scenario, None, 0.0).unwrap();
        assert_eq!(result.weights.len(), 1);
        assert!((result.weights[0] - (-1.0)).abs() < 1e-9, "weights={:?}", result.weights);
        for r in &result.residuals {
            assert!(r.abs() < 1e-9, "residual no nulo: {r}");
        }
        assert!(result.residual_std < 1e-9);
        assert!(result.residual_max_abs < 1e-9);
        assert!(result.cost.is_none());
        assert!(result.gross_notional.is_none());
        assert!(result.residual_greeks.is_none());
    }

    #[test]
    fn cost_is_sum_of_abs_weight_times_price() {
        let design = vec![vec![1.0, 0.0], vec![0.0, 1.0], vec![1.0, 1.0]];
        let target = vec![1.0, 2.0, 3.0];
        let prices = [1.5, 2.5];

        let result = solve_hedge(&design, &target, Some(&prices), 0.01).unwrap();
        let expected_cost: f64 =
            result.weights.iter().zip(prices.iter()).map(|(w, p)| w.abs() * p).sum();
        assert!(result.cost.is_some());
        assert!((result.cost.unwrap() - expected_cost).abs() < 1e-12);
        assert_eq!(result.cost, result.gross_notional);
    }

    #[test]
    fn rejects_mismatched_instrument_prices_length() {
        let design = vec![vec![1.0, 2.0], vec![3.0, 4.0]];
        let target = vec![1.0, 2.0];
        let prices = [1.0]; // longitud 1, pero hay 2 instrumentos
        let err = solve_hedge(&design, &target, Some(&prices), 0.0).unwrap_err();
        assert!(err.contains("instrument_prices"), "mensaje inesperado: {err}");
    }

    #[test]
    fn max_gross_notional_scales_weights_down_uniformly() {
        let design = vec![vec![1.0, 0.0], vec![0.0, 1.0], vec![1.0, 1.0], vec![-1.0, 1.0]];
        let target = vec![1.0, 2.0, 3.0, -1.0];
        let prices = [1.0, 1.0];

        let unconstrained = solve_hedge(&design, &target, Some(&prices), 0.01).unwrap();
        let unconstrained_gross = unconstrained.gross_notional.unwrap();
        let cap = unconstrained_gross / 2.0;

        let constraints = HedgeConstraints { bounds: None, max_gross_notional: Some(cap) };
        let scaled = solve_hedge_with_constraints(&design, &target, Some(&prices), 0.01, &constraints).unwrap();
        assert!((scaled.gross_notional.unwrap() - cap).abs() < 1e-6, "gross={:?}", scaled.gross_notional);
        for (w_scaled, w_base) in scaled.weights.iter().zip(unconstrained.weights.iter()) {
            assert!((w_scaled - w_base * 0.5).abs() < 1e-6, "scaled={:?} base={:?}", scaled.weights, unconstrained.weights);
        }
    }

    #[test]
    fn max_gross_notional_without_prices_is_rejected() {
        let design = vec![vec![1.0]];
        let target = vec![1.0];
        let constraints = HedgeConstraints { bounds: None, max_gross_notional: Some(1.0) };
        let err = solve_hedge_with_constraints(&design, &target, None, 0.0, &constraints).unwrap_err();
        assert!(err.contains("instrument_prices"), "mensaje inesperado: {err}");
    }

    #[test]
    fn incompatible_liquidity_and_bounds_are_rejected_explicitly() {
        // Un unico instrumento, optimo sin restricciones w=1.0 (columna=target). bounds fuerza
        // el peso a permanecer en 1.0 (lower==upper==1.0), pero max_gross_notional pide reducirlo
        // -- las dos restricciones son incompatibles y deben fallar, no romperse en silencio.
        let target = vec![2.0, 4.0];
        let design: Vec<Vec<f64>> = target.iter().map(|&t| vec![t]).collect();
        let prices = [10.0];
        let constraints =
            HedgeConstraints { bounds: Some(vec![(1.0, 1.0)]), max_gross_notional: Some(1.0) };
        let err = solve_hedge_with_constraints(&design, &target, Some(&prices), 0.0, &constraints).unwrap_err();
        assert!(err.contains("incompatibles"), "mensaje inesperado: {err}");
    }

    // -----------------------------------------------------------------------------------------
    // synthesize_hedge_gbm_q: integracion end-to-end sobre contratos JSON `engine.payoff/v1`.
    // -----------------------------------------------------------------------------------------

    fn call_json(strike: f64, maturity: f64) -> String {
        format!(
            r#"{{
                "schema": "engine.payoff/v1",
                "id": "CALL",
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
                                "left": {{"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}},
                                "right": {{"type": "constant", "value": {strike}}}
                            }},
                            "right": {{"type": "constant", "value": 0.0}}
                        }}
                    }}
                }}
            }}"#
        )
    }

    #[test]
    fn hedging_a_call_with_the_same_call_gives_weight_minus_one_and_no_mc_noise() {
        // Target e instrumento son el MISMO contrato, evaluados sobre las MISMAS rutas simuladas
        // (una unica simulacion compartida, ver el doc-comment de la funcion) -- este es un
        // ajuste ALGEBRAICO exacto (design == target columna a columna), sin ruido Monte Carlo de
        // por medio: no hace falta ninguna tolerancia estadistica, weight debe salir -1 y
        // residual_std debe ser (numericamente) cero.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let spec = call_json(100.0, 1.0);
        let result = synthesize_hedge_gbm_q(
            "cpu",
            &spec,
            std::slice::from_ref(&spec),
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &HedgeConstraints::default(),
            false,
            5_000,
            7,
        )
        .unwrap();

        assert_eq!(result.weights.len(), 1);
        assert!((result.weights[0] - (-1.0)).abs() < 1e-6, "weights={:?}", result.weights);
        assert!(result.residual_std < 1e-6, "residual_std={}", result.residual_std);
        assert!(result.residual_max_abs < 1e-6, "residual_max_abs={}", result.residual_max_abs);
    }

    #[test]
    fn duplicated_instruments_need_ridge_to_solve_cleanly() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let target_spec = call_json(100.0, 1.0);
        let instrument_spec = call_json(90.0, 1.0);
        let instruments = vec![instrument_spec.clone(), instrument_spec];

        // Con ridge > 0 el solve debe tener exito SIEMPRE y devolver pesos finitos (dos
        // instrumentos identicos => el reparto entre ambos no esta determinado de forma unica,
        // no se verifica ningun valor concreto, solo que el sistema resuelve limpio).
        let result = synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &instruments,
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.05,
            &HedgeConstraints::default(),
            false,
            5_000,
            11,
        )
        .unwrap();
        assert_eq!(result.weights.len(), 2);
        assert!(result.weights.iter().all(|w| w.is_finite()), "weights={:?}", result.weights);

        // Con ridge=0.0 documentamos que puede fallar o degenerar (columnas identicas): no se
        // afirma cual de las dos cosas pasa, solo que si sale Ok los pesos son finitos.
        match synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &instruments,
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &HedgeConstraints::default(),
            false,
            5_000,
            11,
        ) {
            Ok(result) => assert!(result.weights.iter().all(|w| w.is_finite())),
            Err(msg) => assert!(msg.contains("pivote"), "mensaje inesperado: {msg}"),
        }
    }

    #[test]
    fn instrument_prices_produce_a_cost_matching_the_recomputed_sum() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let target_spec = call_json(100.0, 1.0);
        let instruments = vec![call_json(90.0, 1.0), call_json(110.0, 1.0)];
        let prices = [12.0, 4.0];

        let result = synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &instruments,
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            Some(&prices),
            0.001,
            &HedgeConstraints::default(),
            false,
            5_000,
            13,
        )
        .unwrap();

        let expected_cost: f64 =
            result.weights.iter().zip(prices.iter()).map(|(w, p)| w.abs() * p).sum();
        assert!(result.cost.is_some());
        assert!((result.cost.unwrap() - expected_cost).abs() < 1e-9);
    }

    #[test]
    fn preflight_rejects_a_target_referencing_an_observable_the_model_does_not_generate() {
        let target_spec = call_json(100.0, 1.0).replace("EQ.SPOT.XYZ", "EQ.SPOT.OTHER");
        let instrument_spec = call_json(100.0, 1.0);
        let err = synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &[instrument_spec],
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &HedgeConstraints::default(),
            false,
            1_000,
            7,
        )
        .expect_err("un target que referencia un observable distinto debe fallar en preflight");
        assert!(err.contains("EQ.SPOT.OTHER"));
        assert!(err.contains("no generado"));
    }

    #[test]
    fn preflight_rejects_an_instrument_referencing_an_observable_the_model_does_not_generate() {
        let target_spec = call_json(100.0, 1.0);
        let bad_instrument = call_json(100.0, 1.0).replace("EQ.SPOT.XYZ", "EQ.SPOT.OTHER");
        let err = synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &[bad_instrument],
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &HedgeConstraints::default(),
            false,
            1_000,
            7,
        )
        .expect_err("un instrumento que referencia un observable distinto debe fallar en preflight");
        assert!(err.contains("EQ.SPOT.OTHER"));
        assert!(err.contains("no generado"));
    }

    #[test]
    fn rejects_an_empty_instrument_universe() {
        let target_spec = call_json(100.0, 1.0);
        let err = synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &[],
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &HedgeConstraints::default(),
            false,
            1_000,
            7,
        )
        .expect_err("un universo de cobertura vacio debe fallar");
        assert!(err.contains("vacio"));
    }

    #[test]
    fn rejects_zero_paths() {
        let target_spec = call_json(100.0, 1.0);
        let instrument_spec = call_json(100.0, 1.0);
        let err = synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &[instrument_spec],
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &HedgeConstraints::default(),
            false,
            0,
            7,
        )
        .expect_err("n_paths=0 debe fallar");
        assert!(err.contains("n_paths"));
    }

    #[test]
    fn residual_greeks_of_hedging_a_call_with_itself_are_all_near_zero() {
        // Mismo caso que hedging_a_call_with_the_same_call_gives_weight_minus_one_and_no_mc_noise
        // (weight=-1, cobertura algebraicamente exacta): el portfolio cubierto (target - 1*target)
        // es identicamente cero en TODA ruta, sea cual sea el parametro bumpeado -- todas las
        // Greeks residuales deben salir (numericamente) cero.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let spec = call_json(100.0, 1.0);
        let result = synthesize_hedge_gbm_q(
            "cpu",
            &spec,
            std::slice::from_ref(&spec),
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &HedgeConstraints::default(),
            true,
            5_000,
            7,
        )
        .unwrap();

        let greeks = result.residual_greeks.expect("se pidio compute_residual_greeks=true");
        assert!(greeks.delta.abs() < 1e-6, "greeks={greeks:?}");
        assert!(greeks.rho.abs() < 1e-6, "greeks={greeks:?}");
        assert!(greeks.dividend_yield.abs() < 1e-6, "greeks={greeks:?}");
        assert!(greeks.vega.abs() < 1e-6, "greeks={greeks:?}");
    }

    #[test]
    fn bounds_pin_the_weight_of_an_unhedgeable_instrument_to_a_fixed_position() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let target_spec = call_json(100.0, 1.0);
        let instrument_spec = call_json(90.0, 1.0);

        let constraints = HedgeConstraints { bounds: Some(vec![(-0.1, -0.1)]), max_gross_notional: None };
        let result = synthesize_hedge_gbm_q(
            "cpu",
            &target_spec,
            &[instrument_spec],
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
            &constraints,
            false,
            2_000,
            5,
        )
        .unwrap();
        assert_eq!(result.weights.len(), 1);
        assert!((result.weights[0] - (-0.1)).abs() < 1e-6, "weights={:?}", result.weights);
    }
}
