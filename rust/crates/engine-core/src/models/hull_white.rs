//! Modelo Hull-White de 1 factor para la curva de tipos (PLAN.md §5.2), caso base del
//! prototipo (IRS + Hull-White).
//!
//! **Simplificación deliberada de esta primera versión**: en vez de un `theta(t)`
//! dependiente del tiempo calibrado a una curva forward inicial (lo que exigiría la
//! infraestructura de calibración de Fase 2 — registry de curvas, bootstrapping), se usa
//! un nivel de reversión de largo plazo `b` constante:
//!
//! `dr_t = a * (b - r_t) dt + sigma dW_t`
//!
//! Esto es exactamente el modelo de Vasicek, matemáticamente el caso particular de
//! Hull-White 1F con `theta(t) = a*b` constante. Conserva la misma dinámica
//! mean-reverting y la misma familia de fórmulas cerradas afines para bonos cero-cupón
//! (necesarias para validar la simulación Monte Carlo, PLAN.md §5.6 capa 2), a coste de
//! no reproducir una curva de mercado arbitraria — eso se aborda calibrando `theta(t)` en
//! una fase posterior sin cambiar esta estructura (el registry de modelos, Fase 2, es
//! precisamente el punto de extensión para ello).
//!
//! **Vectorizado sobre paths, no sobre escalares**: `r_t` y los parámetros son tensores
//! Burn de forma `[n_paths]` (o `[1]`, que Burn difunde contra `[n_paths]`), de forma que
//! una única llamada a `simulate_path` simula *todas* las trayectorias Monte Carlo a la
//! vez como operaciones tensoriales — el backend (`crate::backend::CpuBackend` o, tras la
//! feature `gpu`, `GpuBackend`) decide si eso corre vectorizado en CPU o en GPU, sin que
//! este módulo lo sepa (PLAN.md §5.1).

use crate::kernel::euler_maruyama_step;
use crate::models::ShortRateModel;
use burn::tensor::backend::Backend;
use burn::tensor::Tensor;

/// Parámetros del modelo, genéricos sobre `B: Backend`. Instanciar con
/// `B = Autodiff<CpuBackend>` y parámetros marcados `.require_grad()` propaga
/// sensibilidades a través de la simulación y la valoración (PLAN.md §5.3).
#[derive(Debug, Clone)]
pub struct HullWhite1F<B: Backend> {
    /// Velocidad de reversión a la media. Forma `[1]` (o `[n_paths]` si se quisiera un
    /// parámetro distinto por path, no usado en esta primera versión).
    pub a: Tensor<B, 1>,
    /// Nivel de reversión de largo plazo (simplificación constante, ver docs del módulo).
    pub b: Tensor<B, 1>,
    /// Volatilidad del tipo corto.
    pub sigma: Tensor<B, 1>,
}

impl<B: Backend> HullWhite1F<B> {
    pub fn new(a: Tensor<B, 1>, b: Tensor<B, 1>, sigma: Tensor<B, 1>) -> Self {
        Self { a, b, sigma }
    }

    /// `B(t,T) = (1 - exp(-a*(T-t))) / a`.
    fn b_factor(&self, tau: f64) -> Tensor<B, 1> {
        let neg_a_tau = self.a.clone().mul_scalar(-tau);
        let one_minus_exp = neg_a_tau.exp().neg().add_scalar(1.0);
        one_minus_exp.div(self.a.clone())
    }

    /// `A(t,T)` del precio afín del bono cero-cupón bajo Vasicek/Hull-White de parámetros
    /// constantes (Brigo-Mercurio, "Interest Rate Models", ec. 3.9).
    fn a_factor(&self, tau: f64, b_t_t: Tensor<B, 1>) -> Tensor<B, 1> {
        let a2 = self.a.clone() * self.a.clone();
        let sigma2 = self.sigma.clone() * self.sigma.clone();

        let term1 = (b_t_t.clone().add_scalar(-tau))
            * (a2.clone() * self.b.clone() - sigma2.clone().mul_scalar(0.5))
            / a2.clone();
        let term2 = (sigma2 * b_t_t.clone() * b_t_t) / self.a.clone().mul_scalar(4.0);

        (term1 - term2).exp()
    }

    /// Precio analítico del bono cero-cupón `P(t,T)` dado el tipo corto `r_t` observado en
    /// `t`, bajo la fórmula cerrada afín del modelo. Sirve como referencia de correctitud
    /// para la simulación Monte Carlo (PLAN.md §5.6 capa 2) y como bloque de construcción
    /// de la valoración del IRS (`crate::products::irs`).
    pub fn zero_coupon_bond(&self, r_t: Tensor<B, 1>, t: f64, maturity: f64) -> Tensor<B, 1> {
        let tau = maturity - t;
        let b_t_t = self.b_factor(tau);
        let a_t_t = self.a_factor(tau, b_t_t.clone());
        a_t_t * (b_t_t * r_t).neg().exp()
    }

    /// Simula, vectorizado sobre todos los paths a la vez, la trayectoria del tipo corto
    /// `r_t` por discretización de Euler-Maruyama (`crate::kernel::euler_maruyama_step`).
    /// `shocks[i]` tiene forma `[n_paths]` (un shock normal estándar por path en el paso
    /// `i`); devuelve `shocks.len() + 1` tensores (incluyendo `r0`), cada uno forma
    /// `[n_paths]`.
    pub fn simulate_path(&self, r0: Tensor<B, 1>, dt: f64, shocks: &[Tensor<B, 1>]) -> Vec<Tensor<B, 1>> {
        let mut path = Vec::with_capacity(shocks.len() + 1);
        let mut r = r0;
        path.push(r.clone());
        for shock in shocks {
            let drift = self.a.clone() * (self.b.clone() - r.clone());
            r = euler_maruyama_step(r, drift, self.sigma.clone(), dt, shock.clone());
            path.push(r.clone());
        }
        path
    }
}

/// Implementación de la interfaz homogénea `ShortRateModel` (PLAN.md §7.16): el estado de
/// `HullWhite1F` es directamente el tipo corto `r_t`.
impl<B: Backend> ShortRateModel<B> for HullWhite1F<B> {
    type State = Tensor<B, 1>;

    fn zero_coupon_bond(&self, state: Self::State, t: f64, maturity: f64) -> Tensor<B, 1> {
        HullWhite1F::zero_coupon_bond(self, state, t, maturity)
    }
}

/// Factor de descuento estocástico `exp(-∫_0^T r_s ds)` a lo largo de una trayectoria
/// simulada (vectorizado sobre paths), aproximando la integral por la regla del trapecio
/// (mismo orden de precisión que la discretización de Euler-Maruyama usada para generar
/// la trayectoria). Promediar este factor sobre paths estima `P(0,T)` vía Monte Carlo.
pub fn path_discount_factor<B: Backend>(path: &[Tensor<B, 1>], dt: f64) -> Tensor<B, 1> {
    let mut integral = path[0].clone().mul_scalar(0.0);
    for window in path.windows(2) {
        integral = integral + (window[0].clone() + window[1].clone()).mul_scalar(0.5 * dt);
    }
    integral.neg().exp()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::CpuBackend;
    use burn::tensor::{Distribution, TensorData};

    type Device = burn::tensor::Device<CpuBackend>;

    fn scalar(value: f64) -> Tensor<CpuBackend, 1> {
        Tensor::from_data(TensorData::from([value]), &Device::default())
    }

    fn to_f64(t: Tensor<CpuBackend, 1>) -> f64 {
        t.into_data().to_vec::<f64>().unwrap()[0]
    }

    fn reference_model() -> HullWhite1F<CpuBackend> {
        // a, b, sigma y r0 típicos de un caso de prueba de tipos (no calibrado a mercado
        // real, solo un punto de referencia razonable para validar la fórmula/simulación).
        HullWhite1F::new(scalar(0.1), scalar(0.03), scalar(0.01))
    }

    #[test]
    fn zero_coupon_bond_at_zero_maturity_is_one() {
        let model = reference_model();
        let p = model.zero_coupon_bond(scalar(0.02), 1.0, 1.0);
        assert!((to_f64(p) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn zero_coupon_bond_is_decreasing_in_maturity() {
        let model = reference_model();
        let p1 = to_f64(model.zero_coupon_bond(scalar(0.02), 0.0, 1.0));
        let p5 = to_f64(model.zero_coupon_bond(scalar(0.02), 0.0, 5.0));
        let p10 = to_f64(model.zero_coupon_bond(scalar(0.02), 0.0, 10.0));
        assert!(p1 > p5 && p5 > p10);
    }

    /// PLAN.md §5.6 capa 2: convergencia Monte Carlo vs fórmula cerrada. Simula el tipo
    /// corto (vectorizado sobre `n_paths` a la vez) bajo el modelo, calcula el factor de
    /// descuento estocástico por trayectoria y compara su media (estimador MC de P(0,T))
    /// contra el precio analítico, dentro de una tolerancia derivada del error estándar
    /// del estimador (no un número arbitrario).
    #[test]
    fn monte_carlo_bond_price_converges_to_analytic_formula() {
        let model = reference_model();
        let r0 = 0.02;
        let maturity = 5.0;
        let n_steps = 250;
        let n_paths = 50_000;
        let dt = maturity / n_steps as f64;
        let device = Device::default();

        let shocks: Vec<Tensor<CpuBackend, 1>> = (0..n_steps)
            .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), &device))
            .collect();

        let path = model.simulate_path(scalar(r0), dt, &shocks);
        let discounts = path_discount_factor(&path, dt);

        let mc_price = to_f64(discounts.clone().mean());
        let variance = to_f64(
            (discounts.clone() - discounts.clone().mean())
                .powf_scalar(2.0)
                .mean(),
        );
        let std_error = (variance / n_paths as f64).sqrt();

        let analytic_price = to_f64(model.zero_coupon_bond(scalar(r0), 0.0, maturity));

        // Margen generoso (8 errores estándar) para evitar tests flaky mantieniendo la
        // tolerancia atada a la incertidumbre real del estimador, no a un número fijo.
        let tolerance = 8.0 * std_error;
        assert!(
            (mc_price - analytic_price).abs() < tolerance,
            "MC={mc_price} analytic={analytic_price} tol={tolerance} (std_error={std_error})"
        );
    }
}
