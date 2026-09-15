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
//! **Que hace y que no hace esta fase**: implementa el paso 3 (evaluar payoff sobre una rejilla
//! comun de escenarios) y el paso 4 (resolver pesos que minimicen error, con ridge en vez de
//! restricciones explicitas) de la lista de 5 pasos de §11; el paso 5 (riesgo residual) se reporta
//! como `HedgeResult::residuals`/`residual_std`/`residual_max_abs`. Fuera de alcance deliberado
//! (no oculto, PLAN_PRODUCTS.md §16): Greeks residuales, riesgo de base/correlacion/volatilidad
//! explicito, coste de transaccion mas alla del termino de prima estatico (`cost`), liquidez, y
//! restricciones de tipo LP/QP (posiciones minimas/maximas, enteros) -- `ridge` es la unica forma
//! de regularizacion de esta fase, no un sustituto de restricciones duras.
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
// Algebra lineal minima: minimos cuadrados regularizados via ecuaciones normales + Cholesky.
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
fn cholesky_decompose(a: &[Vec<f64>]) -> Result<Vec<Vec<f64>>, String> {
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

/// `min_w ||A w - b||^2 + ridge*||w||^2` via ecuaciones normales `(A^T A + ridge*I) w = A^T b`,
/// resueltas por Cholesky (ver el doc-comment del modulo para el porque de esta eleccion frente a
/// SVD/QR/una dependencia externa). `design[s]` es la fila del escenario `s` (longitud = numero de
/// instrumentos, igual para todas las filas); `target[s]` el valor a ajustar en ese escenario.
///
/// `Err` (nunca `NaN`/degradacion silenciosa) si: `design`/`target` estan vacios, sus longitudes
/// no coinciden, alguna fila de `design` tiene longitud distinta de las demas, `design` tiene
/// columnas de longitud 0, `ridge < 0`, o Cholesky encuentra un pivote no positivo.
fn solve_least_squares_normal_equations(design: &[Vec<f64>], target: &[f64], ridge: f64) -> Result<Vec<f64>, String> {
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

// ---------------------------------------------------------------------------------------------
// API de resultado y orquestacion sobre escenarios ya evaluados (§11, pasos 4-5).
// ---------------------------------------------------------------------------------------------

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
}

/// Resuelve `weights` tales que `target[s] + sum_i(weights[i] * design[s][i]) ~= 0` para todo
/// escenario `s` (la cartera de instrumentos NETEA el target) por minimos cuadrados regularizados:
/// `A = design`, se resuelve `A w ~= -target` (minimizar `||target + A w||^2` es identico a
/// minimizar `||A w - (-target)||^2`, de ahi el signo).
///
/// Valida formas ANTES de llamar al solver: todas las filas de `design` deben tener la misma
/// longitud, que ademas debe coincidir con `instrument_prices.len()` si se proporciona.
pub fn solve_hedge(
    design: &[Vec<f64>],
    target: &[f64],
    instrument_prices: Option<&[f64]>,
    ridge: f64,
) -> Result<HedgeResult, String> {
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
    let n_instruments = design[0].len();
    for (idx, row) in design.iter().enumerate() {
        if row.len() != n_instruments {
            return Err(format!(
                "hedge: la fila {idx} de design tiene longitud {} pero se esperaba {n_instruments} \
                 (todas las filas deben tener la misma longitud, una entrada por instrumento)",
                row.len()
            ));
        }
    }
    if let Some(prices) = instrument_prices {
        if prices.len() != n_instruments {
            return Err(format!(
                "hedge: instrument_prices tiene {} elementos pero design tiene {n_instruments} \
                 instrumentos -- deben coincidir",
                prices.len()
            ));
        }
    }

    let neg_target: Vec<f64> = target.iter().map(|t| -t).collect();
    let weights = solve_least_squares_normal_equations(design, &neg_target, ridge)?;

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
    let cost = instrument_prices
        .map(|prices| weights.iter().zip(prices.iter()).map(|(w, p)| w.abs() * p).sum());

    Ok(HedgeResult { weights, residuals, residual_mean, residual_std, residual_max_abs, cost })
}

// ---------------------------------------------------------------------------------------------
// Sintesis de cobertura bajo GBM sobre una rejilla comun de escenarios (§11, pasos 1-3 + 4 via
// `solve_hedge`).
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

/// Sintetiza una cobertura bajo GBM/Q para `target_spec_json` con el universo de instrumentos
/// `instrument_specs_json` (§11, pasos 1-5 completos):
///
/// 1. `Err` si `instrument_specs_json` esta vacio (ningun instrumento con el que cubrir) o si
///    `n_paths == 0`.
/// 2. compila el target y cada instrumento (`payoff::compile::compile`);
/// 3. preflight de observable unico (`payoff::api::check_single_observable`) sobre el target Y
///    cada instrumento -- ninguno puede referenciar un observable que este `Gbm` no genera;
/// 4. rejilla de tiempos COMPARTIDA: union ordenada/deduplicada (tolerancia `1e-9`, mismo patron
///    que `payoff_exposure_profile_gbm_q`) de `required_times()` del target y de TODOS los
///    instrumentos;
/// 5. simula GBM en esa rejilla UNA SOLA VEZ (todas las rutas se reutilizan para el target y para
///    cada instrumento -- es lo que hace que el ajuste de minimos cuadrados no tenga ruido Monte
///    Carlo relativo ENTRE columnas, solo en valor absoluto);
/// 6. por cada ruta y cada contrato (target + instrumentos), valor presente de su ledger
///    (`sum(cf.amount * exp(-r*cf.payment_time))`), con una seed determinista por ruta (ver el
///    doc-comment del modulo para el porque de no reutilizar `bridge_seed_for_path`);
/// 7. construye `target_pv`/`design` (`design[path][instrument]`);
/// 8. `solve_hedge(&design, &target_pv, instrument_prices, ridge)`.
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

    let mut simulation_times: Vec<f64> = target_payoff.required_times();
    for instrument_payoff in &instrument_payoffs {
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

        let (target_ledger, _events) = evaluate_with_events_seeded(&target_payoff, &path, path_seed);
        let target_present_value: f64 =
            target_ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum();
        target_pv.push(target_present_value);

        let mut row = Vec::with_capacity(instrument_payoffs.len());
        for instrument_payoff in &instrument_payoffs {
            let (ledger, _events) = evaluate_with_events_seeded(instrument_payoff, &path, path_seed);
            let pv: f64 = ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum();
            row.push(pv);
        }
        design.push(row);
    }

    solve_hedge(&design, &target_pv, instrument_prices, ridge)
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
    // solve_hedge: semantica de signo y validacion de formas.
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
    }

    #[test]
    fn rejects_mismatched_instrument_prices_length() {
        let design = vec![vec![1.0, 2.0], vec![3.0, 4.0]];
        let target = vec![1.0, 2.0];
        let prices = [1.0]; // longitud 1, pero hay 2 instrumentos
        let err = solve_hedge(&design, &target, Some(&prices), 0.0).unwrap_err();
        assert!(err.contains("instrument_prices"), "mensaje inesperado: {err}");
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
            &[spec.clone()],
            "EQ.SPOT.XYZ",
            100.0,
            0.05,
            0.0,
            0.2,
            None,
            0.0,
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
            0,
            7,
        )
        .expect_err("n_paths=0 debe fallar");
        assert!(err.contains("n_paths"));
    }
}
