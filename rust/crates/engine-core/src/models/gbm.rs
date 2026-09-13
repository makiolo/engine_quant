//! Movimiento geometrico browniano (GBM) bajo la medida neutral al riesgo Q (PLAN_PRODUCTS.md
//! §12 Fase 5: "Black-Scholes/GBM Q como primer modelo equity"). Primer modelo del motor para un
//! observable de tipo `EQ.SPOT.*` -- analogo en estructura a `HullWhite1F`
//! (`crate::models::hull_white`), pero sin implementar `ShortRateModel` (ese trait es especifico
//! de modelos de curva de tipos; GBM genera un spot, no un bono cero-cupon).
//!
//! **Solo Q, nunca P**: los parametros son `r` (tipo libre de riesgo) y `q` (dividend yield), no
//! un drift fisico `mu` -- por construccion, este modelo no tiene ningun parametro que un
//! escenario fisico P pudiera cambiar (PLAN_PRODUCTS.md §12 Fase 5, criterio de aceptacion
//! "cambiar el drift fisico no afecta un precio Q"): la unica forma de obtener un drift distinto
//! de `r - q` seria construir un modelo P separado (Fase 7), nunca reconfigurar este.
//!
//! Vectorizado sobre paths igual que `HullWhite1F`: los parametros y las trayectorias son
//! tensores Burn de forma `[n_paths]` (o `[1]`, que Burn difunde contra `[n_paths]`).

use burn::tensor::backend::Backend;
use burn::tensor::{Distribution, Tensor};

/// Parametros de GBM, genericos sobre `B: Backend`.
#[derive(Debug, Clone)]
pub struct Gbm<B: Backend> {
    pub s0: Tensor<B, 1>,
    pub r: Tensor<B, 1>,
    pub q: Tensor<B, 1>,
    pub sigma: Tensor<B, 1>,
}

impl<B: Backend> Gbm<B> {
    pub fn new(s0: Tensor<B, 1>, r: Tensor<B, 1>, q: Tensor<B, 1>, sigma: Tensor<B, 1>) -> Self {
        Self { s0, r, q, sigma }
    }

    /// Simula, vectorizado sobre todos los paths a la vez, el spot en cada instante de `times`
    /// (estrictamente creciente, todos > 0 -- `S0` en `t=0` ya es conocido, no simulado).
    /// Discretizacion EXACTA (no Euler-Maruyama): el logaritmo de GBM es un browniano aritmetico,
    /// asi que cada incremento `log(S_{t_i}) - log(S_{t_i-1})` es exactamente
    /// `(r - q - sigma^2/2)*dt + sigma*sqrt(dt)*Z` con `Z ~ N(0,1)` independiente entre
    /// intervalos -- sin sesgo de discretizacion, a diferencia de `euler_maruyama_step`
    /// (necesario para Hull-White porque esa dinamica no tiene solucion exacta en un paso).
    /// Devuelve `times.len()` tensores de forma `[n_paths]`, en el mismo orden que `times`.
    pub fn simulate_at_times(
        &self,
        times: &[f64],
        n_paths: usize,
        device: &B::Device,
    ) -> Vec<Tensor<B, 1>> {
        assert!(!times.is_empty(), "gbm: se requiere al menos un instante de simulacion");
        assert!(times[0] > 0.0, "gbm: los instantes deben ser > 0 (S0 ya es conocido en t=0)");
        assert!(
            times.windows(2).all(|w| w[0] < w[1]),
            "gbm: 'times' debe ser estrictamente creciente"
        );

        let half_variance = self.sigma.clone() * self.sigma.clone().mul_scalar(0.5);
        let drift_rate = self.r.clone() - self.q.clone() - half_variance;

        let mut log_s = self.s0.clone().log();
        let mut previous_t = 0.0;
        let mut result = Vec::with_capacity(times.len());
        for &t in times {
            let dt = t - previous_t;
            let shock: Tensor<B, 1> = Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), device);
            let diffusion = self.sigma.clone().mul_scalar(dt.sqrt()) * shock;
            log_s = log_s + drift_rate.clone().mul_scalar(dt) + diffusion;
            result.push(log_s.clone().exp());
            previous_t = t;
        }
        result
    }
}

/// CDF de la normal estandar, aproximacion racional de Abramowitz & Stegun 26.2.17 (error
/// absoluto < 7.5e-8) -- evita anadir una dependencia nueva solo para esta formula (mismo
/// criterio que `calibration::solve_2x2`/`mc::aggregate`).
fn norm_cdf(x: f64) -> f64 {
    let (sign, x) = if x < 0.0 { (-1.0, -x) } else { (1.0, x) };
    const P: f64 = 0.2316419;
    const B1: f64 = 0.319_381_530;
    const B2: f64 = -0.356_563_782;
    const B3: f64 = 1.781_477_937;
    const B4: f64 = -1.821_255_978;
    const B5: f64 = 1.330_274_429;
    let t = 1.0 / (1.0 + P * x);
    let poly = t * (B1 + t * (B2 + t * (B3 + t * (B4 + t * B5))));
    let phi_x = (-x * x / 2.0).exp() / (2.0 * std::f64::consts::PI).sqrt();
    let cdf_for_positive_x = 1.0 - phi_x * poly;
    if sign > 0.0 {
        cdf_for_positive_x
    } else {
        1.0 - cdf_for_positive_x
    }
}

/// Formula cerrada de Black-Scholes-Merton para una call europea bajo Q (dividendo continuo
/// `q`), usada como oraculo de referencia para la convergencia Monte Carlo de `Gbm` (§12 Fase 5:
/// "Monte Carlo converge a Black-Scholes dentro del intervalo estadistico"). Funcion libre en
/// `f64` puro (no generica sobre `Backend`, no depende de ninguna ruta simulada) porque su unico
/// consumidor de produccion es un valor esperado en forma cerrada, no una cantidad pathwise.
pub fn black_scholes_call(s0: f64, strike: f64, r: f64, q: f64, sigma: f64, maturity: f64) -> f64 {
    assert!(maturity > 0.0, "black_scholes_call: maturity debe ser > 0");
    assert!(sigma > 0.0, "black_scholes_call: sigma debe ser > 0");
    let sqrt_t = maturity.sqrt();
    let d1 = ((s0 / strike).ln() + (r - q + 0.5 * sigma * sigma) * maturity) / (sigma * sqrt_t);
    let d2 = d1 - sigma * sqrt_t;
    s0 * (-q * maturity).exp() * norm_cdf(d1) - strike * (-r * maturity).exp() * norm_cdf(d2)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::CpuBackend;
    use burn::tensor::TensorData;

    type Device = burn::tensor::Device<CpuBackend>;

    fn scalar(value: f64) -> Tensor<CpuBackend, 1> {
        Tensor::from_data(TensorData::from([value]), &Device::default())
    }

    fn to_vec(t: Tensor<CpuBackend, 1>) -> Vec<f64> {
        t.into_data().to_vec::<f64>().unwrap()
    }

    // Hull, "Options, Futures and Other Derivatives": S0=42, K=40, r=10%, sigma=20%, T=0.5,
    // sin dividendos -- call europea de referencia, precio de libro de texto c = 4.76.
    #[test]
    fn black_scholes_call_matches_textbook_value() {
        let price = black_scholes_call(42.0, 40.0, 0.10, 0.0, 0.20, 0.5);
        assert!((price - 4.76).abs() < 0.01, "price={price}");
    }

    #[test]
    fn black_scholes_call_decreases_as_dividend_yield_increases() {
        let low_q = black_scholes_call(100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let high_q = black_scholes_call(100.0, 100.0, 0.05, 0.03, 0.2, 1.0);
        assert!(high_q < low_q);
    }

    #[test]
    fn black_scholes_call_increases_with_volatility() {
        let low_vol = black_scholes_call(100.0, 100.0, 0.05, 0.0, 0.1, 1.0);
        let high_vol = black_scholes_call(100.0, 100.0, 0.05, 0.0, 0.3, 1.0);
        assert!(high_vol > low_vol);
    }

    #[test]
    fn simulate_at_times_starts_from_s0_in_expectation() {
        // PLAN.md §7.19: lock de RNG global, ver crate::rng_test_lock.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let model = Gbm::new(scalar(100.0), scalar(0.05), scalar(0.0), scalar(0.2));
        let n_paths = 20_000;
        let paths = model.simulate_at_times(&[1.0], n_paths, &device);
        let spot_t1 = to_vec(paths[0].clone());
        let mean: f64 = spot_t1.iter().sum::<f64>() / n_paths as f64;
        // E_Q[S_T] = S0 * exp((r-q)*T) bajo la medida de riesgo neutral -- invariante de martingala
        // (descontado por el numerario, no del propio S_T) que debe cumplir cualquier simulacion Q
        // correcta de GBM, independientemente del payoff que se vaya a evaluar despues.
        let expected = 100.0 * (0.05_f64).exp();
        let variance: f64 = spot_t1.iter().map(|s| (s - mean).powi(2)).sum::<f64>() / (n_paths as f64 - 1.0);
        let std_error = (variance / n_paths as f64).sqrt();
        assert!((mean - expected).abs() < 6.0 * std_error, "mean={mean} expected={expected} se={std_error}");
    }

    #[test]
    fn monte_carlo_call_price_converges_to_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let n_paths = 200_000;

        let model = Gbm::new(scalar(s0), scalar(r), scalar(q), scalar(sigma));
        let spot_at_maturity = model.simulate_at_times(&[maturity], n_paths, &device).remove(0);
        let payoff = (spot_at_maturity - scalar(strike)).clamp_min(0.0);
        let discounted = payoff.mul_scalar((-r * maturity).exp());

        let samples = to_vec(discounted);
        let estimate = crate::mc::aggregate(&samples, crate::mc::Z_95);
        let analytic = black_scholes_call(s0, strike, r, q, sigma, maturity);

        // Margen generoso (8 errores estandar), mismo criterio que
        // models::hull_white::tests::monte_carlo_bond_price_converges_to_analytic_formula.
        let tolerance = 8.0 * estimate.std_error;
        assert!(
            (estimate.mean - analytic).abs() < tolerance,
            "MC={} analytic={analytic} tol={tolerance} (std_error={})",
            estimate.mean,
            estimate.std_error
        );
    }
}
