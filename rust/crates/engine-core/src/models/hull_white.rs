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

use crate::kernel::euler_maruyama_step;
use crate::scalar::Scalar;

/// Parámetros del modelo, genéricos sobre `T: Scalar` para poder diferenciarlos vía AAD
/// (PLAN.md §5.3): instanciar con `T = Dual` propaga sensibilidades de `a`/`b`/`sigma`/`r0`
/// a través de la simulación y la valoración.
#[derive(Debug, Clone, Copy)]
pub struct HullWhite1F<T: Scalar> {
    /// Velocidad de reversión a la media.
    pub a: T,
    /// Nivel de reversión de largo plazo (simplificación constante, ver docs del módulo).
    pub b: T,
    /// Volatilidad del tipo corto.
    pub sigma: T,
}

impl<T: Scalar> HullWhite1F<T> {
    pub fn new(a: T, b: T, sigma: T) -> Self {
        Self { a, b, sigma }
    }

    /// `B(t,T) = (1 - exp(-a*(T-t))) / a`.
    fn b_factor(&self, tau: f64) -> T {
        let neg_a_tau = -(self.a * T::from_f64(tau));
        (T::one() - neg_a_tau.exp()) / self.a
    }

    /// `A(t,T)` del precio afín del bono cero-cupón bajo Vasicek/Hull-White de parámetros
    /// constantes (Brigo-Mercurio, "Interest Rate Models", ec. 3.9).
    fn a_factor(&self, tau: f64, b_t_t: T) -> T {
        let a2 = self.a * self.a;
        let sigma2 = self.sigma * self.sigma;
        let term1 = (b_t_t - T::from_f64(tau)) * (a2 * self.b - sigma2 * T::from_f64(0.5)) / a2;
        let term2 = sigma2 * b_t_t.powi(2) / (T::from_f64(4.0) * self.a);
        (term1 - term2).exp()
    }

    /// Precio analítico del bono cero-cupón `P(t,T)` dado el tipo corto `r_t` observado en
    /// `t`, bajo la fórmula cerrada afín del modelo. Sirve como referencia de correctitud
    /// para la simulación Monte Carlo (PLAN.md §5.6 capa 2) y como bloque de construcción
    /// de la valoración del IRS (`crate::products::irs`).
    pub fn zero_coupon_bond(&self, r_t: T, t: f64, maturity: f64) -> T {
        let tau = maturity - t;
        let b_t_t = self.b_factor(tau);
        let a_t_t = self.a_factor(tau, b_t_t);
        a_t_t * (-(b_t_t * r_t)).exp()
    }

    /// Simula una trayectoria del tipo corto `r_t` por discretización de Euler-Maruyama
    /// (`crate::kernel::euler_maruyama_step`), con `shocks.len()` pasos de tamaño `dt`.
    /// Devuelve `shocks.len() + 1` valores (incluyendo `r0`).
    pub fn simulate_path(&self, r0: T, dt: f64, shocks: &[f64]) -> Vec<T> {
        let mut path = Vec::with_capacity(shocks.len() + 1);
        let mut r = r0;
        path.push(r);
        for &shock in shocks {
            let drift = self.a * (self.b - r);
            r = euler_maruyama_step(r, drift, self.sigma, dt, shock);
            path.push(r);
        }
        path
    }
}

/// Factor de descuento estocástico `exp(-∫_0^T r_s ds)` a lo largo de una trayectoria
/// simulada, aproximando la integral por la regla del trapecio (mismo orden de precisión
/// que la discretización de Euler-Maruyama usada para generar la trayectoria). Promediar
/// este factor sobre muchas trayectorias estima `P(0,T)` vía Monte Carlo.
pub fn path_discount_factor<T: Scalar>(path: &[T], dt: f64) -> T {
    let mut integral = T::zero();
    for window in path.windows(2) {
        integral = integral + (window[0] + window[1]) * T::from_f64(0.5 * dt);
    }
    (-integral).exp()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::{mean_f64_simd, CpuBackend, ComputeBackend};

    fn reference_model() -> HullWhite1F<f64> {
        // a, b, sigma y r0 típicos de un caso de prueba de tipos (no calibrado a mercado
        // real, solo un punto de referencia razonable para validar la fórmula/simulación).
        HullWhite1F::new(0.1, 0.03, 0.01)
    }

    #[test]
    fn zero_coupon_bond_at_zero_maturity_is_one() {
        let model = reference_model();
        let p = model.zero_coupon_bond(0.02, 1.0, 1.0);
        assert!((p - 1.0).abs() < 1e-12);
    }

    #[test]
    fn zero_coupon_bond_is_decreasing_in_maturity() {
        let model = reference_model();
        let p1 = model.zero_coupon_bond(0.02, 0.0, 1.0);
        let p5 = model.zero_coupon_bond(0.02, 0.0, 5.0);
        let p10 = model.zero_coupon_bond(0.02, 0.0, 10.0);
        assert!(p1 > p5 && p5 > p10);
    }

    /// PLAN.md §5.6 capa 2: convergencia Monte Carlo vs fórmula cerrada. Simula el tipo
    /// corto bajo el modelo, calcula el factor de descuento estocástico por trayectoria y
    /// compara su media (estimador MC de P(0,T)) contra el precio analítico, dentro de una
    /// tolerancia derivada del error estándar del estimador (no un número arbitrario).
    #[test]
    fn monte_carlo_bond_price_converges_to_analytic_formula() {
        let model = reference_model();
        let r0 = 0.02;
        let maturity = 5.0;
        let n_steps = 250;
        let n_paths = 50_000;
        let dt = maturity / n_steps as f64;

        let backend = CpuBackend;
        let shocks = backend.normal_shocks(n_paths, n_steps, 20240609);

        let discounts: Vec<f64> = shocks
            .iter()
            .map(|path_shocks| {
                let path = model.simulate_path(r0, dt, path_shocks);
                path_discount_factor(&path, dt)
            })
            .collect();

        let mc_price = mean_f64_simd(&discounts);
        let variance = discounts
            .iter()
            .map(|d| (d - mc_price).powi(2))
            .sum::<f64>()
            / discounts.len() as f64;
        let std_error = (variance / discounts.len() as f64).sqrt();

        let analytic_price = model.zero_coupon_bond(r0, 0.0, maturity);

        // Margen generoso (8 errores estándar) para evitar tests flaky mantieniendo la
        // tolerancia atada a la incertidumbre real del estimador, no a un número fijo.
        let tolerance = 8.0 * std_error;
        assert!(
            (mc_price - analytic_price).abs() < tolerance,
            "MC={mc_price} analytic={analytic_price} tol={tolerance} (std_error={std_error})"
        );
    }
}
