//! Calibración de modelos a un `crate::market::MarketSnapshot`: encontrar los parámetros que
//! mejor reproducen ese mercado, en vez de elegirlos a mano (PLAN.md §5.2 documentaba esto
//! como la limitación deliberada de `HullWhite1F`: "un `theta(t)` calibrado... exigiría
//! infraestructura de calibración" — esta es esa infraestructura, aunque calibra parámetros
//! constantes, no un `theta(t)` completo que ajuste la curva exactamente).
//!
//! **Dos calibradores, uno por modelo del motor (PLAN.md §7.16/§7.18), cada uno calibrando un
//! subconjunto distinto de parámetros** — no es el mismo código con los nombres cambiados:
//!
//! - `calibrate_hull_white` (`HullWhite1F`): calibra `a` (velocidad de reversión, debe ser
//!   positiva) y `b` (nivel de reversión de largo plazo, sin restricción de signo) — solo `a`
//!   se reparametriza sobre su logaritmo.
//! - `calibrate_hull_white_2f` (`HullWhite2F`/G2++): calibra `a` y `b`, las velocidades de
//!   reversión de *ambos* factores latentes — las dos deben ser positivas (`HullWhite2F::
//!   b_factor` divide por cada una), así que las dos se reparametrizan sobre su logaritmo. A
//!   diferencia de `HullWhite1F`, aquí `b` no es un nivel: en G2++ el nivel de largo plazo lo
//!   fija por completo `r0` (`phi0`, PLAN.md §7.16), y ambos factores decaen hacia 0.
//!
//! **`sigma`/`r0` (y, en G2++, también `eta`/`rho`) no se calibran; se toman como datos de
//! entrada** — decisión tomada tras comprobarlo empíricamente para `HullWhite1F`, no una
//! simplificación *a priori*: calibrar `sigma` contra únicamente el factor de descuento
//! resultó mal condicionado (el optimizador lo colapsaba hacia 0 sin apenas mover el RMSE)
//! porque `sigma` solo entra en el precio del bono cero-cupón vía el término de convexidad
//! `-sigma²/2` de `a_factor` (ver `crate::models::hull_white::HullWhite1F::a_factor`) — un
//! efecto de segundo orden, casi invisible comparado con el efecto de primer orden de `a`/`b`
//! sobre la forma/nivel de la curva. El mismo razonamiento se aplica sin cambios a `eta`/`rho`
//! de G2++ (entran en `variance_term`/`cross_term`, ambos también convexidad de segundo orden)
//! y con más motivo: no hay forma de identificar una correlación `rho` entre dos factores
//! latentes a partir de una única curva de descuento observada, hagan falta o no instrumentos
//! de volatilidad. En la práctica de mercado, la volatilidad (y la correlación, en un modelo
//! multi-factor) de un modelo de tipo corto se calibra contra instrumentos de volatilidad
//! (swaptions, caps) — fuera de alcance de esta iteración (no hay ese tipo de instrumento en
//! `MarketSnapshot` todavía). `a`/`b` sí están bien identificados por la curva (determinan
//! directamente su forma y su nivel de largo plazo) y calibran de forma robusta en ambos
//! modelos.
//!
//! **Gauss-Newton amortiguado (Levenberg-Marquardt) con jacobiana vía AAD**: en vez de
//! diferencias finitas (bump-and-reval) para la jacobiana de los residuos respecto a los
//! parámetros, se reutiliza el autodiff en modo reverse de Burn que ya usa `crate::smoke`/
//! `tests/aad_vs_bump_reval.rs` (PLAN.md §5.3) — la misma infraestructura de sensibilidades,
//! aplicada a un problema distinto (calibrar, no valorar). Solo 2 parámetros libres y un
//! puñado de pillars por mercado típico hacen que "una `backward()` por residuo, por
//! iteración" sea barato. El bucle de optimización en sí (`levenberg_marquardt_2p`) es
//! genérico sobre "cómo se evalúan precios/jacobiana": ambos calibradores le pasan closures
//! propias (cada una construye el modelo Burn que corresponda) en vez de duplicar el bucle de
//! amortiguación — mismo espíritu de extracción que `crate::exposure::ee_pfe`/
//! `unilateral_cva_with_discount` (PLAN.md §7.16): lo que de verdad difiere entre modelos
//! (construir `HullWhite1F` vs. `HullWhite2F`, cuántos parámetros libres necesitan
//! reparametrización) permanece en cada función pública, no en el optimizador.

use crate::backend::{Autodiff, CpuBackend};
use crate::market::MarketSnapshot;
use crate::models::hull_white::HullWhite1F;
use crate::models::hull_white_2f::HullWhite2F;
use burn::tensor::{Tensor, TensorData};

type Device = burn::tensor::Device<CpuBackend>;
type AD = Autodiff<CpuBackend>;

fn scalar<B: burn::tensor::backend::Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
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

/// Resultado interno del optimizador, en el espacio de parámetros que le haya pasado el
/// llamante (posiblemente reparametrizado, ej. `ln(a)` en vez de `a`) — cada calibrador público
/// deshace su propia reparametrización antes de devolver el resultado.
struct LmOutcome {
    p: [f64; 2],
    rmse: f64,
    iterations: u32,
    converged: bool,
}

/// Levenberg-Marquardt genérico sobre exactamente 2 parámetros libres, reutilizado por
/// `calibrate_hull_white` y `calibrate_hull_white_2f` (ver documentación del módulo): no sabe
/// nada de Hull-White, solo de mínimos cuadrados amortiguados.
///
/// - `price_fn(p) -> precios`: evalúa el modelo en `CpuBackend` puro (sin grafo de autodiff),
///   usada para juzgar si un paso candidato de la amortiguación mejora el RMSE.
/// - `residuals_and_jacobian_fn(p) -> (residuos, jacobiana)`: una `backward()` por pillar sobre
///   `Autodiff<CpuBackend>`, calculada una vez por iteración (no por intento de amortiguación).
fn levenberg_marquardt_2p(
    initial: [f64; 2],
    market_prices: &[f64],
    price_fn: impl Fn([f64; 2]) -> Vec<f64>,
    residuals_and_jacobian_fn: impl Fn([f64; 2]) -> (Vec<f64>, Vec<[f64; 2]>),
) -> LmOutcome {
    const MAX_ITERATIONS: u32 = 50;
    const MAX_DAMPING_TRIES: u32 = 20;
    const TOLERANCE: f64 = 1e-12;

    let residuals_of = |prices: &[f64]| -> Vec<f64> {
        prices.iter().zip(market_prices.iter()).map(|(p, m)| p - m).collect()
    };

    let mut p = initial;
    let mut lambda = 1e-3_f64;
    let mut rmse = rmse_of(&residuals_of(&price_fn(p)));
    let mut iterations = 0u32;
    let mut converged = rmse < TOLERANCE;

    while !converged && iterations < MAX_ITERATIONS {
        iterations += 1;

        let (residuals, jacobian) = residuals_and_jacobian_fn(p);

        let mut jtj = [[0.0_f64; 2]; 2];
        let mut jtr = [0.0_f64; 2];
        for (row, &res) in jacobian.iter().zip(residuals.iter()) {
            for i in 0..2 {
                jtr[i] += row[i] * res;
                for j in 0..2 {
                    jtj[i][j] += row[i] * row[j];
                }
            }
        }

        // --- Amortiguación de Levenberg-Marquardt: aceptar el paso solo si de verdad mejora
        // el RMSE, subiendo `lambda` (más parecido a descenso de gradiente, paso más corto)
        // si no, hasta MAX_DAMPING_TRIES veces antes de rendirse en esta iteración. ---
        let mut accepted = false;
        for _ in 0..MAX_DAMPING_TRIES {
            let mut damped = jtj;
            for i in 0..2 {
                damped[i][i] += lambda * jtj[i][i].max(1e-12);
            }
            let Some(delta) = solve_2x2(damped, [-jtr[0], -jtr[1]]) else {
                lambda *= 4.0;
                continue;
            };
            let candidate = [p[0] + delta[0], p[1] + delta[1]];
            let candidate_rmse = rmse_of(&residuals_of(&price_fn(candidate)));

            if candidate_rmse < rmse {
                p = candidate;
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

    LmOutcome { p, rmse, iterations, converged }
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

/// Igual que `HullWhiteCalibrationResult` pero para `HullWhite2F`/G2++ (PLAN.md §7.16/§7.18):
/// `sigma`/`eta`/`rho`/`r0` no se calibran (ver documentación del módulo), se devuelven tal
/// cual se pasaron a `calibrate_hull_white_2f`, para que el resultado sea directamente los seis
/// parámetros que espera `HullWhite2F::new`/`ENGINE.CREATE_MODEL("HullWhite2F", ...)`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HullWhite2FCalibrationResult {
    pub a: f64,
    pub b: f64,
    pub sigma: f64,
    pub eta: f64,
    pub rho: f64,
    pub r0: f64,
    pub rmse: f64,
    pub iterations: u32,
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

/// Equivalente de dos factores de `discount_factors`, con los dos factores latentes en su
/// valor inicial (`x_0 = y_0 = 0`, ver `crate::models::hull_white_2f`).
#[allow(clippy::too_many_arguments)]
fn discount_factors_2f(a: f64, b: f64, sigma: f64, eta: f64, rho: f64, r0: f64, pillars: &[f64]) -> Vec<f64> {
    let device = Device::default();
    let model: HullWhite2F<CpuBackend> = HullWhite2F::new(
        scalar(a, &device),
        scalar(b, &device),
        scalar(sigma, &device),
        scalar(eta, &device),
        rho,
        scalar(r0, &device),
    );
    let zero = scalar(0.0, &device);
    pillars
        .iter()
        .map(|&t| model.zero_coupon_bond(zero.clone(), zero.clone(), 0.0, t).into_scalar())
        .collect()
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

    let device = Device::default();
    let pillars = market.pillars().to_vec();
    let market_prices: Vec<f64> = pillars.iter().map(|&t| market.discount_factor(t)).collect();

    let price_fn = |p: [f64; 2]| discount_factors(p[0].exp(), p[1], sigma, r0, &pillars);
    let residuals_and_jacobian_fn = |p: [f64; 2]| -> (Vec<f64>, Vec<[f64; 2]>) {
        let log_a_t: Tensor<AD, 1> = scalar(p[0], &device).require_grad();
        let b_t: Tensor<AD, 1> = scalar(p[1], &device).require_grad();
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
        (residuals, jacobian)
    };

    let outcome = levenberg_marquardt_2p([initial_a.ln(), initial_b], &market_prices, price_fn, residuals_and_jacobian_fn);

    HullWhiteCalibrationResult {
        a: outcome.p[0].exp(),
        b: outcome.p[1],
        sigma,
        r0,
        rmse: outcome.rmse,
        iterations: outcome.iterations,
        converged: outcome.converged,
    }
}

/// Calibra `a`/`b` de `HullWhite2F` (las velocidades de reversión de los dos factores
/// latentes) a `market` por mínimos cuadrados sobre el factor de descuento en cada pillar,
/// partiendo de `(initial_a, initial_b)`. `sigma`/`eta`/`rho`/`r0` no se calibran (ver
/// documentación del módulo): entran como datos fijos, igual que en `HullWhite2F::new`.
/// `initial_a`/`initial_b` deben ser estrictamente positivos (ambos se reparametrizan
/// internamente sobre su logaritmo — a diferencia de `calibrate_hull_white`, aquí *los dos*
/// parámetros libres son velocidades de reversión, no un nivel de largo plazo).
#[allow(clippy::too_many_arguments)]
pub fn calibrate_hull_white_2f(
    market: &MarketSnapshot,
    initial_a: f64,
    initial_b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
) -> HullWhite2FCalibrationResult {
    assert!(initial_a > 0.0, "initial_a debe ser positivo");
    assert!(initial_b > 0.0, "initial_b debe ser positivo");

    let device = Device::default();
    let pillars = market.pillars().to_vec();
    let market_prices: Vec<f64> = pillars.iter().map(|&t| market.discount_factor(t)).collect();

    let price_fn = |p: [f64; 2]| discount_factors_2f(p[0].exp(), p[1].exp(), sigma, eta, rho, r0, &pillars);
    let residuals_and_jacobian_fn = |p: [f64; 2]| -> (Vec<f64>, Vec<[f64; 2]>) {
        let log_a_t: Tensor<AD, 1> = scalar(p[0], &device).require_grad();
        let log_b_t: Tensor<AD, 1> = scalar(p[1], &device).require_grad();
        let sigma_t: Tensor<AD, 1> = scalar(sigma, &device);
        let eta_t: Tensor<AD, 1> = scalar(eta, &device);
        let r0_t: Tensor<AD, 1> = scalar(r0, &device);
        let zero: Tensor<AD, 1> = scalar(0.0, &device);

        let mut residuals = Vec::with_capacity(pillars.len());
        let mut jacobian = Vec::with_capacity(pillars.len());
        for (i, &t) in pillars.iter().enumerate() {
            // Mismo motivo que en calibrate_hull_white: el modelo (y sus nodos a_t/b_t=exp(.))
            // se reconstruye en cada pillar porque Burn libera el grafo de cómputo tras la
            // primera backward() que lo atraviesa.
            let model: HullWhite2F<AD> =
                HullWhite2F::new(log_a_t.clone().exp(), log_b_t.clone().exp(), sigma_t.clone(), eta_t.clone(), rho, r0_t.clone());
            let price = model.zero_coupon_bond(zero.clone(), zero.clone(), 0.0, t);
            residuals.push(price.clone().into_scalar() - market_prices[i]);
            let grads = price.backward();
            jacobian.push([log_a_t.grad(&grads).unwrap().into_scalar(), log_b_t.grad(&grads).unwrap().into_scalar()]);
        }
        (residuals, jacobian)
    };

    let outcome = levenberg_marquardt_2p(
        [initial_a.ln(), initial_b.ln()],
        &market_prices,
        price_fn,
        residuals_and_jacobian_fn,
    );

    HullWhite2FCalibrationResult {
        a: outcome.p[0].exp(),
        b: outcome.p[1].exp(),
        sigma,
        eta,
        rho,
        r0,
        rmse: outcome.rmse,
        iterations: outcome.iterations,
        converged: outcome.converged,
    }
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

    #[test]
    fn calibration_2f_recovers_known_parameters_from_a_synthetic_market() {
        let (true_a, true_b, sigma, eta, rho, r0) = (0.15, 0.25, 0.008, 0.01, -0.6, 0.02);
        let pillars = vec![0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0];
        let market = MarketSnapshot::synthetic_from_hull_white_2f(true_a, true_b, sigma, eta, rho, r0, pillars);

        // Estimación inicial deliberadamente lejos de los parámetros "verdaderos", igual que
        // el test equivalente de HullWhite1F.
        let result = calibrate_hull_white_2f(&market, 0.4, 0.05, sigma, eta, rho, r0);

        assert!(result.converged, "no convergió: rmse={} iterations={}", result.rmse, result.iterations);
        assert!(result.rmse < 1e-9, "rmse demasiado alto: {}", result.rmse);
        assert!((result.a - true_a).abs() < 1e-4, "a={} esperado~{true_a}", result.a);
        assert!((result.b - true_b).abs() < 1e-4, "b={} esperado~{true_b}", result.b);
        assert_eq!(result.sigma, sigma, "sigma no se calibra, debe devolverse tal cual");
        assert_eq!(result.eta, eta, "eta no se calibra, debe devolverse tal cual");
        assert_eq!(result.rho, rho, "rho no se calibra, debe devolverse tal cual");
        assert_eq!(result.r0, r0, "r0 no se calibra, debe devolverse tal cual");
    }

    #[test]
    fn calibration_2f_from_the_true_parameters_is_already_converged() {
        let (a, b, sigma, eta, rho, r0) = (0.1, 0.2, 0.01, 0.012, -0.7, 0.03);
        let market = MarketSnapshot::synthetic_from_hull_white_2f(a, b, sigma, eta, rho, r0, vec![1.0, 5.0, 10.0]);

        let result = calibrate_hull_white_2f(&market, a, b, sigma, eta, rho, r0);

        assert!(result.converged);
        assert!(result.rmse < 1e-9);
    }
}
