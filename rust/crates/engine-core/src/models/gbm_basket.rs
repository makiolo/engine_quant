//! Movimiento geometrico browniano MULTI-ACTIVO correlacionado bajo Q (PLAN_IMPROVE_NOTEBOOK.md
//! Fase 3, §2 "Modelo multi-activo correlacionado"): generaliza `crate::models::gbm::Gbm` (un
//! unico observable) a `n_assets` observables que comparten el mismo browniano bajo una matriz de
//! correlacion instantanea constante -- primer modelo del motor para baskets/spreads/worst-of/
//! quanto (PLAN_IMPROVE_NOTEBOOK.md §1, friccion 3).
//!
//! **Vectorizacion elegida: `Vec<Tensor<B,1>>` (uno por activo), no `Tensor<B,2>`**. El plan de
//! esta fase dejaba ambas representaciones abiertas ("`Vec<Tensor<B,1>>` o un unico `Tensor<B,2>`
//! de forma `[n_assets]`"); se eligio la primera porque generaliza DIRECTAMENTE el patron ya
//! probado de `models::hull_white_2f::HullWhite2F::simulate_path` (dos brownianos correlacionados
//! combinados a mano via `dW2 = rho*dW1 + sqrt(1-rho^2)*shocks_y`, es decir, una combinacion
//! lineal escalar de shocks independientes por Cholesky de una matriz 2x2) sin introducir ninguna
//! API de tensor 2D (`matmul`/`reshape` con broadcasting `[1,n]` vs `[n_paths,n]`) que este crate
//! no usa hoy en ningun otro sitio -- el Cholesky de la matriz de correlacion N x N
//! (`payoff::hedge::cholesky_decompose`, reutilizado tal cual, PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2
//! punto 3) se calcula UNA VEZ en `f64` puro en la construccion, y cada paso de tiempo combina los
//! `n_assets` tensores de shocks independientes `Tensor<B,1>` (forma `[n_paths]`) con los
//! coeficientes de esa factorizacion via `mul_scalar`/`+`, exactamente como `HullWhite2F` -- la
//! vectorizacion real sigue ocurriendo en la dimension de PATHS (cada `Tensor<B,1>` mueve
//! `n_paths` escalares a la vez), no se pierde nada frente a una representacion `Tensor<B,2>`.
//!
//! Parametros (`s0`/`r`/`q`/`sigma`) uno por activo, cada uno un `Tensor<B,1>` de forma `[1]`
//! (broadcastable contra `[n_paths]`, mismo patron que `Gbm::s0` etc.) -- `Vec<Tensor<B,1>>` de
//! longitud `n_assets`, NUNCA un unico tensor `[n_assets]`, para poder reusar `mul_scalar`/`log`/
//! `exp` elemento a elemento sobre cada activo por separado sin reshapes.
//!
//! **Solo Q, nunca P** (igual que `Gbm`): `r`/`q` por activo, no un drift fisico -- ver el
//! doc-comment de `models::gbm` para el razonamiento completo, identico aqui por activo.

use crate::payoff::hedge::cholesky_decompose;
use burn::tensor::backend::Backend;
use burn::tensor::{Distribution, Tensor};

/// Parametros de un basket GBM de `n_assets` activos, genericos sobre `B: Backend`.
#[derive(Debug, Clone)]
pub struct GbmBasket<B: Backend> {
    pub s0: Vec<Tensor<B, 1>>,
    pub r: Vec<Tensor<B, 1>>,
    pub q: Vec<Tensor<B, 1>>,
    pub sigma: Vec<Tensor<B, 1>>,
    /// Matriz de correlacion original (fila a fila, `n_assets x n_assets`) tal como se paso a
    /// `new` -- se conserva solo para diagnostico/`to_params`, la simulacion usa
    /// `cholesky_lower`.
    pub correlation: Vec<Vec<f64>>,
    n_assets: usize,
    /// `L` tal que `L * L^T = correlation` (Cholesky, PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2 punto
    /// 3): calculado UNA VEZ en la construccion, no en cada llamada a `simulate_at_times` (la
    /// matriz de correlacion no cambia por ruta ni por paso de tiempo).
    cholesky_lower: Vec<Vec<f64>>,
}

impl<B: Backend> GbmBasket<B> {
    /// `Err` si `s0`/`r`/`q`/`sigma` no tienen todos la misma longitud que `correlation`
    /// (`n x n`), o si `correlation` no es definida positiva (Cholesky, ver
    /// `payoff::hedge::cholesky_decompose`) -- la validacion de SIMETRIA vive en la capa Python/
    /// C++ (`engine_typed.model.GbmBasket`, PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2 ultimo punto), pero
    /// una matriz no simetrica que aun asi fuera "casualmente" definida positiva pasaria este
    /// constructor sin avisar -- documentado, no oculto: este constructor es la ultima linea de
    /// defensa contra una matriz NO PSD (nunca degrada a NaN, mismo criterio que
    /// `hedge::solve_hedge`), no contra la falta de simetria.
    pub fn new(
        s0: Vec<Tensor<B, 1>>,
        r: Vec<Tensor<B, 1>>,
        q: Vec<Tensor<B, 1>>,
        sigma: Vec<Tensor<B, 1>>,
        correlation: Vec<Vec<f64>>,
    ) -> Result<Self, String> {
        let n_assets = s0.len();
        if n_assets == 0 {
            return Err("gbm_basket: se requiere al menos un activo (s0 vacio)".to_string());
        }
        if r.len() != n_assets || q.len() != n_assets || sigma.len() != n_assets {
            return Err(format!(
                "gbm_basket: s0/r/q/sigma deben tener la misma longitud (s0={}, r={}, q={}, sigma={})",
                n_assets,
                r.len(),
                q.len(),
                sigma.len()
            ));
        }
        if correlation.len() != n_assets || correlation.iter().any(|row| row.len() != n_assets) {
            return Err(format!(
                "gbm_basket: 'correlation' debe ser una matriz {n}x{n} (uno por activo, {n} activos declarados)",
                n = n_assets
            ));
        }
        let cholesky_lower = cholesky_decompose(&correlation).map_err(|e| {
            format!(
                "gbm_basket: la matriz de correlacion no es definida positiva (Cholesky fallo): {e}"
            )
        })?;
        Ok(Self { s0, r, q, sigma, correlation, n_assets, cholesky_lower })
    }

    pub fn n_assets(&self) -> usize {
        self.n_assets
    }

    /// Simula, vectorizado sobre todos los paths a la vez (una `Tensor<B,1>` de forma `[n_paths]`
    /// por activo y por instante), el spot de los `n_assets` activos en cada instante de `times`
    /// (mismas restricciones que `Gbm::simulate_at_times`: estrictamente creciente, todos > 0).
    /// Discretizacion EXACTA por activo (igual que `Gbm`, el log de cada GBM marginal es un
    /// browniano aritmetico sin sesgo de discretizacion); la correlacion entre activos se aplica
    /// combinando `n_assets` shocks normales INDEPENDIENTES con los coeficientes de
    /// `cholesky_lower` antes de escalar por `sigma`, exactamente el mismo patron de
    /// `HullWhite2F::simulate_path` generalizado de 2 a `n_assets` factores.
    ///
    /// Devuelve `times.len()` vectores de `n_assets` tensores `[n_paths]` cada uno --
    /// `result[time_idx][asset_idx]` (ver el doc-comment del modulo para por que esta forma, no
    /// `Tensor<B,2>`).
    pub fn simulate_at_times(
        &self,
        times: &[f64],
        n_paths: usize,
        device: &B::Device,
    ) -> Vec<Vec<Tensor<B, 1>>> {
        assert!(!times.is_empty(), "gbm_basket: se requiere al menos un instante de simulacion");
        assert!(times[0] > 0.0, "gbm_basket: los instantes deben ser > 0 (S0 ya es conocido en t=0)");
        assert!(
            times.windows(2).all(|w| w[0] < w[1]),
            "gbm_basket: 'times' debe ser estrictamente creciente"
        );
        let n = self.n_assets;

        let drift_rate: Vec<Tensor<B, 1>> = (0..n)
            .map(|i| {
                let half_variance = self.sigma[i].clone() * self.sigma[i].clone().mul_scalar(0.5);
                self.r[i].clone() - self.q[i].clone() - half_variance
            })
            .collect();

        let mut log_s: Vec<Tensor<B, 1>> = self.s0.iter().map(|s| s.clone().log()).collect();
        let mut previous_t = 0.0;
        let mut result = Vec::with_capacity(times.len());
        for &t in times {
            let dt = t - previous_t;
            // Shocks normales independientes, uno por activo -- la correlacion NO esta aqui
            // todavia (ver mas abajo).
            let independent: Vec<Tensor<B, 1>> = (0..n)
                .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), device))
                .collect();
            // Correlacionar: correlated[j] = sum_k L[j][k] * independent[k] (L*Z tiene
            // covarianza L*L^T = correlation cuando Z es independiente estandar) -- misma
            // construccion que `HullWhite2F::simulate_path` para el caso 2x2, generalizada a
            // n_assets via Cholesky en vez de la formula cerrada rho/sqrt(1-rho^2).
            let correlated: Vec<Tensor<B, 1>> = (0..n)
                .map(|j| {
                    let mut acc = independent[0].clone().mul_scalar(self.cholesky_lower[j][0]);
                    for (k, indep_k) in independent.iter().enumerate().take(n).skip(1) {
                        acc = acc + indep_k.clone().mul_scalar(self.cholesky_lower[j][k]);
                    }
                    acc
                })
                .collect();

            let mut step_values = Vec::with_capacity(n);
            for j in 0..n {
                let diffusion = self.sigma[j].clone().mul_scalar(dt.sqrt()) * correlated[j].clone();
                log_s[j] = log_s[j].clone() + drift_rate[j].clone().mul_scalar(dt) + diffusion;
                step_values.push(log_s[j].clone().exp());
            }
            result.push(step_values);
            previous_t = t;
        }
        result
    }
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

    fn mean_var(xs: &[f64]) -> (f64, f64) {
        let n = xs.len() as f64;
        let mean = xs.iter().sum::<f64>() / n;
        let var = xs.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / (n - 1.0);
        (mean, var)
    }

    #[test]
    fn rejects_mismatched_lengths() {
        let err = GbmBasket::<CpuBackend>::new(
            vec![scalar(100.0), scalar(100.0)],
            vec![scalar(0.05)],
            vec![scalar(0.0), scalar(0.0)],
            vec![scalar(0.2), scalar(0.2)],
            vec![vec![1.0, 0.0], vec![0.0, 1.0]],
        )
        .expect_err("longitudes de r distintas de s0 deberian rechazarse");
        assert!(err.contains("misma longitud"), "err={err}");
    }

    #[test]
    fn rejects_non_square_correlation() {
        let err = GbmBasket::<CpuBackend>::new(
            vec![scalar(100.0), scalar(100.0)],
            vec![scalar(0.05), scalar(0.05)],
            vec![scalar(0.0), scalar(0.0)],
            vec![scalar(0.2), scalar(0.2)],
            vec![vec![1.0, 0.0, 0.0], vec![0.0, 1.0, 0.0]],
        )
        .expect_err("correlation 2x3 deberia rechazarse");
        assert!(err.contains("2x2") || err.contains("matriz"), "err={err}");
    }

    #[test]
    fn rejects_non_positive_definite_correlation() {
        // rho=1.5 no es una correlacion valida -- Cholesky de [[1,1.5],[1.5,1]] falla (pivote
        // negativo en la segunda fila: 1 - 1.5^2 < 0).
        let err = GbmBasket::<CpuBackend>::new(
            vec![scalar(100.0), scalar(100.0)],
            vec![scalar(0.05), scalar(0.05)],
            vec![scalar(0.0), scalar(0.0)],
            vec![scalar(0.2), scalar(0.2)],
            vec![vec![1.0, 1.5], vec![1.5, 1.0]],
        )
        .expect_err("correlation no PSD deberia rechazarse (Cholesky)");
        assert!(err.contains("Cholesky") || err.contains("definida positiva"), "err={err}");
    }

    #[test]
    fn marginal_moments_match_univariate_gbm_regardless_of_correlation() {
        // PLAN_IMPROVE_NOTEBOOK.md Fase 3, criterio de aceptacion del modelo (Rust): la media/
        // varianza MARGINAL de cada activo del basket debe coincidir con un GBM univariante con
        // los mismos s0/r/q/sigma -- la correlacion solo afecta la relacion ENTRE activos, nunca
        // la distribucion marginal de uno solo.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let (s0, r, q, sigma, t) = (100.0, 0.05, 0.0, 0.2, 1.0);
        let n_paths = 50_000;

        let basket = GbmBasket::<CpuBackend>::new(
            vec![scalar(s0), scalar(s0)],
            vec![scalar(r), scalar(r)],
            vec![scalar(q), scalar(q)],
            vec![scalar(sigma), scalar(sigma)],
            vec![vec![1.0, 0.7], vec![0.7, 1.0]],
        )
        .unwrap();
        let paths = basket.simulate_at_times(&[t], n_paths, &device);
        let asset0 = to_vec(paths[0][0].clone());

        let (mean, var) = mean_var(&asset0);
        let expected_mean = s0 * ((r - q) * t).exp();
        let std_error = (var / n_paths as f64).sqrt();
        assert!(
            (mean - expected_mean).abs() < 6.0 * std_error,
            "mean={mean} expected={expected_mean} se={std_error}"
        );
    }

    #[test]
    fn log_return_covariance_matches_analytic_formula() {
        // Cov(log S1_T, log S2_T) = rho*sigma1*sigma2*T (lognormal correlacionada estandar,
        // PLAN_IMPROVE_NOTEBOOK.md Fase 3, criterio de aceptacion Rust explicito).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let (s0_1, s0_2) = (100.0, 50.0);
        let (r, q) = (0.03, 0.0);
        let (sigma1, sigma2) = (0.2, 0.35);
        let rho = 0.6;
        let t = 2.0;
        let n_paths = 80_000;

        let basket = GbmBasket::<CpuBackend>::new(
            vec![scalar(s0_1), scalar(s0_2)],
            vec![scalar(r), scalar(r)],
            vec![scalar(q), scalar(q)],
            vec![scalar(sigma1), scalar(sigma2)],
            vec![vec![1.0, rho], vec![rho, 1.0]],
        )
        .unwrap();
        let paths = basket.simulate_at_times(&[t], n_paths, &device);
        let asset0 = to_vec(paths[0][0].clone());
        let asset1 = to_vec(paths[0][1].clone());

        let log0: Vec<f64> = asset0.iter().map(|s| s.ln()).collect();
        let log1: Vec<f64> = asset1.iter().map(|s| s.ln()).collect();
        let (mean0, _) = mean_var(&log0);
        let (mean1, _) = mean_var(&log1);
        let n = n_paths as f64;
        let cov: f64 =
            log0.iter().zip(log1.iter()).map(|(a, b)| (a - mean0) * (b - mean1)).sum::<f64>() / (n - 1.0);

        let expected_cov = rho * sigma1 * sigma2 * t;
        // Error estandar de una covarianza empirica sobre n_paths muestras aprox
        // sigma1*sigma2*sqrt((1+rho^2)/n) (aproximacion asintotica estandar) -- margen generoso
        // (8x) mismo criterio que el resto de tests Monte Carlo del crate.
        let cov_std_error = sigma1 * sigma2 * ((1.0 + rho * rho) / n).sqrt() * t.sqrt() * t.sqrt();
        let tolerance = 8.0 * cov_std_error;
        assert!(
            (cov - expected_cov).abs() < tolerance,
            "cov={cov} expected={expected_cov} tol={tolerance}"
        );
    }

    #[test]
    fn zero_correlation_gives_near_zero_empirical_covariance() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let n_paths = 80_000;
        let (s0, r, q, sigma, t) = (100.0, 0.03, 0.0, 0.25, 1.0);

        let basket = GbmBasket::<CpuBackend>::new(
            vec![scalar(s0), scalar(s0)],
            vec![scalar(r), scalar(r)],
            vec![scalar(q), scalar(q)],
            vec![scalar(sigma), scalar(sigma)],
            vec![vec![1.0, 0.0], vec![0.0, 1.0]],
        )
        .unwrap();
        let paths = basket.simulate_at_times(&[t], n_paths, &device);
        let log0: Vec<f64> = to_vec(paths[0][0].clone()).iter().map(|s| s.ln()).collect();
        let log1: Vec<f64> = to_vec(paths[0][1].clone()).iter().map(|s| s.ln()).collect();
        let (mean0, _) = mean_var(&log0);
        let (mean1, _) = mean_var(&log1);
        let n = n_paths as f64;
        let cov: f64 =
            log0.iter().zip(log1.iter()).map(|(a, b)| (a - mean0) * (b - mean1)).sum::<f64>() / (n - 1.0);
        let cov_std_error = sigma * sigma * (1.0_f64 / n).sqrt() * t;
        assert!(cov.abs() < 8.0 * cov_std_error, "cov={cov} tol={}", 8.0 * cov_std_error);
    }
}
