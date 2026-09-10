//! Calibración de modelos a un `crate::market::MarketSnapshot`: encontrar los parámetros que
//! mejor reproducen ese mercado, en vez de elegirlos a mano (PLAN.md §5.2 documentaba esto
//! como la limitación deliberada de `HullWhite1F`: "un `theta(t)` calibrado... exigiría
//! infraestructura de calibración" — esta es esa infraestructura, aunque calibra `a`/`b`
//! constantes, no un `theta(t)` completo que ajuste la curva exactamente).
//!
//! **Solo `a`/`b` se calibran; `sigma` se toma como dato de entrada, igual que `r0`** —
//! decisión tomada tras comprobarlo empíricamente, no una simplificación *a priori*: calibrar
//! `sigma` contra únicamente el factor de descuento resultó mal condicionado (el optimizador
//! lo colapsaba hacia 0 sin apenas mover el RMSE) porque `sigma` solo entra en el precio del
//! bono cero-cupón vía el término de convexidad `-sigma²/2` de `a_factor` (ver
//! `crate::models::hull_white::HullWhite1F::a_factor`) — un efecto de segundo orden, casi
//! invisible comparado con el efecto de primer orden de `a`/`b` sobre la forma/nivel de la
//! curva. Esto no es un defecto del optimizador: en la práctica de mercado, la volatilidad de
//! un modelo de tipo corto se calibra contra instrumentos de volatilidad (swaptions, caps),
//! no contra la curva de descuento — fuera de alcance de esta iteración (no hay ese tipo de
//! instrumento en `MarketSnapshot` todavía). `a`/`b` sí están bien identificados por la curva
//! (determinan directamente su forma y su nivel de largo plazo) y calibran de forma robusta.
//!
//! **Gauss-Newton amortiguado (Levenberg-Marquardt) con jacobiana vía AAD**: en vez de
//! diferencias finitas (bump-and-reval) para la jacobiana de los residuos respecto a los
//! parámetros, se reutiliza el autodiff en modo reverse de Burn que ya usa `crate::smoke`/
//! `tests/aad_vs_bump_reval.rs` (PLAN.md §5.3) — la misma infraestructura de sensibilidades,
//! aplicada a un problema distinto (calibrar, no valorar). Solo 2 parámetros libres y un
//! puñado de pillars por mercado típico hacen que "una `backward()` por residuo, por
//! iteración" sea barato.
//!
//! `a` debe ser positivo para que la fórmula afín de `HullWhite1F` tenga sentido (`b_factor`
//! divide por `a`); en vez de restringir el optimizador, se reparametriza internamente sobre
//! `ln(a)` (variable libre, sin restricción) y se recupera `a = exp(ln_a)` al final — el
//! propio autodiff se encarga de la regla de la cadena a través de `exp()`, sin álgebra
//! manual.

use crate::backend::{Autodiff, CpuBackend};
use crate::market::MarketSnapshot;
use crate::models::hull_white::HullWhite1F;
use burn::tensor::{Tensor, TensorData};

type Device = burn::tensor::Device<CpuBackend>;
type AD = Autodiff<CpuBackend>;

fn scalar<B: burn::tensor::backend::Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Parámetros óptimos de un `HullWhite1F` que mejor reproducen (en mínimos cuadrados sobre el
/// factor de descuento) un `MarketSnapshot`, más diagnóstico del ajuste. `sigma`/`r0` no se
/// calibran (ver documentación del módulo): se devuelven tal cual se pasaron a
/// `calibrate_hull_white`, para que el resultado sea directamente los cuatro parámetros que
/// espera `HullWhite1F::new`/`ENGINE.CREATE_MODEL("HullWhite1F", ...)`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HullWhiteCalibrationResult {
    pub a: f64,
    pub b: f64,
    pub sigma: f64,
    pub r0: f64,
    /// Raíz del error cuadrático medio de los residuos de precio (`P_modelo - P_mercado`) en
    /// el óptimo encontrado — 0 exacto no es alcanzable en general: `HullWhite1F` con `a`/`b`
    /// constantes (Vasicek, ver `crate::models::hull_white`) no tiene grados de libertad
    /// suficientes para reproducir una curva de mercado arbitraria pillar a pillar.
    pub rmse: f64,
    pub iterations: u32,
    /// `true` si `rmse` bajó de la tolerancia interna antes de agotar `max_iterations`.
    pub converged: bool,
}

/// Factor de descuento bajo `HullWhite1F(a,b,sigma)` en `CpuBackend` puro (sin grafo de
/// autodiff) — usado para evaluar candidatos dentro del bucle de amortiguación de
/// Levenberg-Marquardt sin recalcular la jacobiana en cada intento.
fn discount_factors(a: f64, b: f64, sigma: f64, r0: f64, pillars: &[f64]) -> Vec<f64> {
    let device = Device::default();
    let model: HullWhite1F<CpuBackend> = HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let r0_t = scalar(r0, &device);
    pillars
        .iter()
        .map(|&t| model.zero_coupon_bond(r0_t.clone(), 0.0, t).into_scalar())
        .collect()
}

fn rmse_of(residuals: &[f64]) -> f64 {
    (residuals.iter().map(|r| r * r).sum::<f64>() / residuals.len() as f64).sqrt()
}

/// Resuelve `m * x = v` para una matriz 2x2 por la regla de Cramer; `None` si `m` es singular
/// (dentro de una tolerancia laxa) — suficiente para 2 incógnitas, no hace falta una
/// dependencia de álgebra lineal para esto.
fn solve_2x2(m: [[f64; 2]; 2], v: [f64; 2]) -> Option<[f64; 2]> {
    let det = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    if det.abs() < 1e-14 {
        return None;
    }
    let x0 = (v[0] * m[1][1] - m[0][1] * v[1]) / det;
    let x1 = (m[0][0] * v[1] - v[0] * m[1][0]) / det;
    Some([x0, x1])
}

/// Calibra `a`/`b` de `HullWhite1F` a `market` por mínimos cuadrados sobre el factor de
/// descuento en cada pillar, partiendo de `(initial_a, initial_b)`. `sigma`/`r0` no se
/// calibran (ver documentación del módulo): entran como datos fijos, igual que en
/// `HullWhite1F::new`. `initial_a` debe ser estrictamente positivo (se reparametriza
/// internamente sobre su logaritmo, ver documentación del módulo).
pub fn calibrate_hull_white(
    market: &MarketSnapshot,
    initial_a: f64,
    initial_b: f64,
    sigma: f64,
    r0: f64,
) -> HullWhiteCalibrationResult {
    assert!(initial_a > 0.0, "initial_a debe ser positivo");

    const MAX_ITERATIONS: u32 = 50;
    const MAX_DAMPING_TRIES: u32 = 20;
    const TOLERANCE: f64 = 1e-12;

    let device = Device::default();
    let pillars = market.pillars().to_vec();
    let market_prices: Vec<f64> = pillars.iter().map(|&t| market.discount_factor(t)).collect();

    let mut log_a = initial_a.ln();
    let mut b = initial_b;
    let mut lambda = 1e-3_f64;

    let params_to_prices = |log_a: f64, b: f64| discount_factors(log_a.exp(), b, sigma, r0, &pillars);
    let residuals_of = |prices: &[f64]| -> Vec<f64> {
        prices.iter().zip(market_prices.iter()).map(|(p, m)| p - m).collect()
    };

    let mut rmse = rmse_of(&residuals_of(&params_to_prices(log_a, b)));
    let mut iterations = 0u32;
    let mut converged = rmse < TOLERANCE;

    while !converged && iterations < MAX_ITERATIONS {
        iterations += 1;

        // --- Jacobiana vía AAD: una backward() por pillar, respecto a (ln_a, b). ---
        let log_a_t: Tensor<AD, 1> = scalar(log_a, &device).require_grad();
        let b_t: Tensor<AD, 1> = scalar(b, &device).require_grad();
        let sigma_t: Tensor<AD, 1> = scalar(sigma, &device);
        let r0_t: Tensor<AD, 1> = scalar(r0, &device);

        let mut residuals = Vec::with_capacity(pillars.len());
        let mut jacobian = Vec::with_capacity(pillars.len());
        for (i, &t) in pillars.iter().enumerate() {
            // Reconstruye el modelo (y por tanto el nodo a_t=exp(log_a_t)) en cada pillar:
            // reutilizar el mismo nodo intermedio entre varias llamadas a backward() no
            // funciona (Burn libera el grafo de cómputo tras la primera backward() que lo
            // atraviesa, así que la segunda ya no encuentra el camino de vuelta hasta la
            // hoja -- grad() devolvía None). Las hojas (log_a_t/b_t) sí se pueden clonar y
            // reutilizar entre pasadas independientes sin este problema.
            let model: HullWhite1F<AD> = HullWhite1F::new(log_a_t.clone().exp(), b_t.clone(), sigma_t.clone());
            let price = model.zero_coupon_bond(r0_t.clone(), 0.0, t);
            residuals.push(price.clone().into_scalar() - market_prices[i]);
            let grads = price.backward();
            jacobian.push([log_a_t.grad(&grads).unwrap().into_scalar(), b_t.grad(&grads).unwrap().into_scalar()]);
        }

        let mut jtj = [[0.0_f64; 2]; 2];
        let mut jtr = [0.0_f64; 2];
        for (row, &res) in jacobian.iter().zip(residuals.iter()) {
            for p in 0..2 {
                jtr[p] += row[p] * res;
                for q in 0..2 {
                    jtj[p][q] += row[p] * row[q];
                }
            }
        }

        // --- Amortiguación de Levenberg-Marquardt: aceptar el paso solo si de verdad mejora
        // el RMSE, subiendo `lambda` (más parecido a descenso de gradiente, paso más corto)
        // si no, hasta MAX_DAMPING_TRIES veces antes de rendirse en esta iteración. ---
        let mut accepted = false;
        for _ in 0..MAX_DAMPING_TRIES {
            let mut damped = jtj;
            for p in 0..2 {
                damped[p][p] += lambda * jtj[p][p].max(1e-12);
            }
            let Some(delta) = solve_2x2(damped, [-jtr[0], -jtr[1]]) else {
                lambda *= 4.0;
                continue;
            };
            let (candidate_log_a, candidate_b) = (log_a + delta[0], b + delta[1]);
            let candidate_rmse = rmse_of(&residuals_of(&params_to_prices(candidate_log_a, candidate_b)));

            if candidate_rmse < rmse {
                log_a = candidate_log_a;
                b = candidate_b;
                rmse = candidate_rmse;
                lambda = (lambda * 0.5).max(1e-12);
                accepted = true;
                break;
            }
            lambda *= 4.0;
        }

        converged = rmse < TOLERANCE;
        if !accepted {
            break; // atascado: ni amortiguando mucho se encontró un paso que mejore
        }
    }

    HullWhiteCalibrationResult { a: log_a.exp(), b, sigma, r0, rmse, iterations, converged }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calibration_recovers_known_parameters_from_a_synthetic_market() {
        let (true_a, true_b, sigma, r0) = (0.15, 0.025, 0.008, 0.02);
        let pillars = vec![0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0];
        let market = MarketSnapshot::synthetic_from_hull_white(true_a, true_b, sigma, r0, pillars);

        // Estimación inicial deliberadamente lejos de los parámetros "verdaderos" -- si el
        // optimizador solo funcionase partiendo ya del óptimo, no probaría gran cosa.
        let result = calibrate_hull_white(&market, 0.3, 0.01, sigma, r0);

        assert!(result.converged, "no convergió: rmse={} iterations={}", result.rmse, result.iterations);
        assert!(result.rmse < 1e-9, "rmse demasiado alto: {}", result.rmse);
        assert!((result.a - true_a).abs() < 1e-4, "a={} esperado~{true_a}", result.a);
        assert!((result.b - true_b).abs() < 1e-4, "b={} esperado~{true_b}", result.b);
        assert_eq!(result.sigma, sigma, "sigma no se calibra, debe devolverse tal cual");
        assert_eq!(result.r0, r0, "r0 no se calibra, debe devolverse tal cual");
    }

    #[test]
    fn calibration_from_the_true_parameters_is_already_converged() {
        // Punto de partida = óptimo: debe detectar convergencia casi de inmediato (rmse ~ 0
        // ya en la primera evaluación, antes de iterar).
        let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
        let market = MarketSnapshot::synthetic_from_hull_white(a, b, sigma, r0, vec![1.0, 5.0, 10.0]);

        let result = calibrate_hull_white(&market, a, b, sigma, r0);

        assert!(result.converged);
        assert!(result.rmse < 1e-9);
    }

    #[test]
    fn calibration_of_a_flat_curve_recovers_a_flat_long_run_level() {
        // Una curva plana (mismo zero rate en todos los pillars) bajo Hull-White corresponde
        // aproximadamente (no exactamente: la convexidad de sigma > 0 impide un ajuste
        // perfecto con solo (a,b) constantes, de ahí que no se exija `result.converged` aquí)
        // a b ~= r0 -- el tipo corto no tiene por qué revertir a ningún otro sitio si la curva
        // ya está en su nivel de largo plazo. Caso de mercado "fabricado a mano" directo, no
        // vía `synthetic_from_hull_white`.
        let market = MarketSnapshot::new(vec![1.0, 5.0, 10.0, 20.0], vec![0.03, 0.03, 0.03, 0.03]);
        let result = calibrate_hull_white(&market, 0.1, 0.01, 0.01, 0.03);

        assert!(result.rmse < 1e-6, "ajuste demasiado pobre: rmse={}", result.rmse);
        assert!((result.b - 0.03).abs() < 1e-3, "b={} esperado~0.03", result.b);
    }
}
