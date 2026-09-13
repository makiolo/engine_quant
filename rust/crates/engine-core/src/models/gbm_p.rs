//! Movimiento geometrico browniano (GBM) bajo la medida fisica P (PLAN_PRODUCTS.md §12 Fase 7:
//! "configuracion fisica separada -- drift, distribucion, calibracion/estimacion"). Estructura
//! deliberadamente paralela a `crate::models::gbm::Gbm` (Q), pero NO es ese tipo reconfigurado:
//! son dos structs distintos porque `Gbm` documenta explicitamente que su unica forma de drift es
//! `r - q` y que "la unica forma de obtener un drift distinto ... seria construir un modelo P
//! separado (Fase 7), nunca reconfigurar este" -- ver el doc-comment de ese modulo. Reutilizar
//! `Gbm` con `r=mu, q=0` habria colado un parametro fisico en un tipo cuyo contrato documentado
//! es "solo Q, nunca P", exactamente lo que esa nota de Fase 5 queria impedir.
//!
//! `mu` es el drift TOTAL esperado del log-retorno (no libre de riesgo, no ajustado por
//! dividendo): `E_P[S_T] = S0 * exp(mu * T)`. No hay parametro `q` porque bajo P un dividend
//! yield no es una cantidad separada del propio drift esperado -- si hace falta modelarlo por
//! separado en una fase posterior, sera un campo nuevo de este struct, nunca una reinterpretacion
//! de `r`/`q` de `Gbm`.

use burn::tensor::backend::Backend;
use burn::tensor::{Distribution, Tensor};

/// Parametros de GBM bajo P, genericos sobre `B: Backend`.
#[derive(Debug, Clone)]
pub struct GbmP<B: Backend> {
    pub s0: Tensor<B, 1>,
    pub mu: Tensor<B, 1>,
    pub sigma: Tensor<B, 1>,
}

impl<B: Backend> GbmP<B> {
    pub fn new(s0: Tensor<B, 1>, mu: Tensor<B, 1>, sigma: Tensor<B, 1>) -> Self {
        Self { s0, mu, sigma }
    }

    /// Identica discretizacion EXACTA que `Gbm::simulate_at_times` (el log-precio sigue siendo un
    /// browniano aritmetico bajo cualquier medida, solo cambia el drift) -- unica diferencia real:
    /// `drift_rate = mu - sigma^2/2` en vez de `r - q - sigma^2/2`. Ver ese metodo para el detalle
    /// de las precondiciones (`times` > 0 estrictamente creciente).
    pub fn simulate_at_times(
        &self,
        times: &[f64],
        n_paths: usize,
        device: &B::Device,
    ) -> Vec<Tensor<B, 1>> {
        assert!(!times.is_empty(), "gbm_p: se requiere al menos un instante de simulacion");
        assert!(times[0] > 0.0, "gbm_p: los instantes deben ser > 0 (S0 ya es conocido en t=0)");
        assert!(
            times.windows(2).all(|w| w[0] < w[1]),
            "gbm_p: 'times' debe ser estrictamente creciente"
        );

        let half_variance = self.sigma.clone() * self.sigma.clone().mul_scalar(0.5);
        let drift_rate = self.mu.clone() - half_variance;

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

    #[test]
    fn simulate_at_times_matches_the_expected_value_formula_e_p_s_t_equals_s0_exp_mu_t() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let (s0, mu, sigma) = (100.0, 0.30, 0.2);
        let model = GbmP::new(scalar(s0), scalar(mu), scalar(sigma));
        let n_paths = 50_000;

        let paths = model.simulate_at_times(&[1.0], n_paths, &device);
        let spot_t1 = to_vec(paths[0].clone());
        let mean: f64 = spot_t1.iter().sum::<f64>() / n_paths as f64;
        let expected = s0 * mu.exp();
        let variance: f64 = spot_t1.iter().map(|s| (s - mean).powi(2)).sum::<f64>() / (n_paths as f64 - 1.0);
        let std_error = (variance / n_paths as f64).sqrt();
        assert!((mean - expected).abs() < 6.0 * std_error, "mean={mean} expected={expected} se={std_error}");
    }

    #[test]
    fn a_higher_physical_drift_produces_a_higher_expected_terminal_spot() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let device = Device::default();
        let n_paths = 50_000;

        let low_drift = GbmP::new(scalar(100.0), scalar(0.02), scalar(0.2));
        let high_drift = GbmP::new(scalar(100.0), scalar(0.30), scalar(0.2));

        let low_mean: f64 = {
            let v = to_vec(low_drift.simulate_at_times(&[1.0], n_paths, &device).remove(0));
            v.iter().sum::<f64>() / n_paths as f64
        };
        let high_mean: f64 = {
            let v = to_vec(high_drift.simulate_at_times(&[1.0], n_paths, &device).remove(0));
            v.iter().sum::<f64>() / n_paths as f64
        };
        assert!(high_mean > low_mean, "high_mean={high_mean} deberia ser > low_mean={low_mean}");
    }
}
