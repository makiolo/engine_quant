//! Kernels numéricos genéricos, independientes de cualquier modelo o producto concreto
//! (PLAN.md §3.1: "primitivas numéricas — paths, mallas, integración, RNG, reducciones").

use crate::scalar::Scalar;

/// Malla temporal uniforme `[0, t1, ..., t_max]` con `n_steps` pasos de tamaño `dt`.
#[derive(Debug, Clone)]
pub struct TimeGrid {
    pub dt: f64,
    pub n_steps: usize,
}

impl TimeGrid {
    pub fn new(t_max: f64, n_steps: usize) -> Self {
        assert!(n_steps > 0, "n_steps debe ser > 0");
        Self {
            dt: t_max / n_steps as f64,
            n_steps,
        }
    }

    /// Instantes `t_0=0, t_1, ..., t_{n_steps}=t_max` (longitud `n_steps + 1`).
    pub fn times(&self) -> Vec<f64> {
        (0..=self.n_steps).map(|i| i as f64 * self.dt).collect()
    }
}

/// Un paso de discretización de Euler-Maruyama para una EDE escalar
/// `dx_t = drift(x_t) dt + diffusion(x_t) dW_t`:
///
/// `x_{t+dt} = x_t + drift * dt + diffusion * dW`, con `dW = shock * sqrt(dt)` para un
/// shock normal estándar `shock ~ N(0,1)`.
///
/// Genérico sobre `T: Scalar`: `x`, `drift` y `diffusion` pueden ser `f64` (simulación
/// pura) o `Dual` (para propagar sensibilidades de parámetros del modelo, PLAN.md §5.3).
/// El shock aleatorio y `dt` se mantienen como `f64` deliberadamente: el ruido de Monte
/// Carlo no se diferencia, solo la parte determinista del recorrido de cómputo.
pub fn euler_maruyama_step<T: Scalar>(x: T, drift: T, diffusion: T, dt: f64, shock: f64) -> T {
    let dw = shock * dt.sqrt();
    x + drift * T::from_f64(dt) + diffusion * T::from_f64(dw)
}

/// Media aritmética de un slice de valores, genérica sobre `T: Scalar`.
pub fn mean<T: Scalar>(values: &[T]) -> T {
    if values.is_empty() {
        return T::zero();
    }
    let sum: T = values.iter().copied().sum();
    sum / T::from_f64(values.len() as f64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn time_grid_has_expected_times() {
        let grid = TimeGrid::new(2.0, 4);
        assert_eq!(grid.dt, 0.5);
        assert_eq!(grid.times(), vec![0.0, 0.5, 1.0, 1.5, 2.0]);
    }

    #[test]
    fn euler_step_zero_shock_is_pure_drift() {
        // dx = 0.1*x dt, con shock=0 no hay difusión: x1 = x0 + drift*dt.
        let x0 = 1.0_f64;
        let x1 = euler_maruyama_step(x0, 0.1 * x0, 0.0, 0.25, 0.0);
        assert!((x1 - 1.025).abs() < 1e-12);
    }

    #[test]
    fn euler_step_diffusion_scales_with_sqrt_dt() {
        let dt = 0.25_f64;
        let x1 = euler_maruyama_step(0.0_f64, 0.0, 1.0, dt, 1.0);
        assert!((x1 - dt.sqrt()).abs() < 1e-12);
    }

    #[test]
    fn mean_of_deterministic_values() {
        let values = vec![1.0_f64, 2.0, 3.0, 4.0];
        assert_eq!(mean(&values), 2.5);
    }

    #[test]
    fn mean_of_empty_slice_is_zero() {
        let values: Vec<f64> = vec![];
        assert_eq!(mean(&values), 0.0);
    }
}
