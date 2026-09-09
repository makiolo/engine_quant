//! Kernels numéricos genéricos, independientes de cualquier modelo o producto concreto
//! (PLAN.md §3.1: "primitivas numéricas — paths, mallas, integración, RNG, reducciones").
//! Genéricos sobre `B: Backend` (Burn, ver `crate::backend`): la misma función sirve para
//! valoración pura o para propagar sensibilidades, según si `B` lleva o no el envoltorio
//! `Autodiff` (PLAN.md §5.3).

use burn::tensor::backend::Backend;
use burn::tensor::Tensor;

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

/// Un paso de discretización de Euler-Maruyama para una EDE vectorizada sobre paths
/// `dx_t = drift(x_t) dt + diffusion(x_t) dW_t`:
///
/// `x_{t+dt} = x_t + drift * dt + diffusion * dW`, con `dW = shock * sqrt(dt)` para un
/// tensor de shocks normales estándar `shock ~ N(0,1)` (uno por path, forma `[n_paths]`).
///
/// `x`, `drift` y `diffusion` tienen forma `[n_paths]` (o `[1]`, que Burn difunde contra
/// `[n_paths]`); `dt` es un escalar Rust deliberadamente, no un tensor: el paso temporal
/// no es una cantidad que tenga sentido diferenciar.
pub fn euler_maruyama_step<B: Backend>(
    x: Tensor<B, 1>,
    drift: Tensor<B, 1>,
    diffusion: Tensor<B, 1>,
    dt: f64,
    shock: Tensor<B, 1>,
) -> Tensor<B, 1> {
    let dw = shock.mul_scalar(dt.sqrt());
    x + drift.mul_scalar(dt) + diffusion * dw
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::CpuBackend;
    use burn::tensor::TensorData;

    fn t1(value: f64) -> Tensor<CpuBackend, 1> {
        Tensor::from_data(TensorData::from([value]), &Default::default())
    }

    fn to_scalar(t: Tensor<CpuBackend, 1>) -> f64 {
        t.into_data().to_vec::<f64>().unwrap()[0]
    }

    #[test]
    fn time_grid_has_expected_times() {
        let grid = TimeGrid::new(2.0, 4);
        assert_eq!(grid.dt, 0.5);
        assert_eq!(grid.times(), vec![0.0, 0.5, 1.0, 1.5, 2.0]);
    }

    #[test]
    fn euler_step_zero_shock_is_pure_drift() {
        // dx = 0.1*x dt, con shock=0 no hay difusión: x1 = x0 + drift*dt.
        let x1 = euler_maruyama_step(t1(1.0), t1(0.1), t1(0.0), 0.25, t1(0.0));
        assert!((to_scalar(x1) - 1.025).abs() < 1e-12);
    }

    #[test]
    fn euler_step_diffusion_scales_with_sqrt_dt() {
        let dt = 0.25_f64;
        let x1 = euler_maruyama_step(t1(0.0), t1(0.0), t1(1.0), dt, t1(1.0));
        assert!((to_scalar(x1) - dt.sqrt()).abs() < 1e-12);
    }
}
