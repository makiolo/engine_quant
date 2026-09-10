//! Modelo Hull-White de 2 factores (G2++, Brigo-Mercurio "Interest Rate Models", cap. 4)
//! para la curva de tipos -- segundo modelo del motor tras `HullWhite1F` (PLAN.md §7.5,
//! §7.16), mismo tipo de simplificación deliberada: en vez de calibrar `phi(t)` a una curva
//! forward inicial, se usa un desplazamiento constante `phi0` (aquí, el parámetro `r0`, ya
//! que con los dos factores latentes arrancando en cero el tipo corto inicial es
//! precisamente `phi0`):
//!
//! `dx_t = -a*x_t dt + sigma dW1_t`,  `x_0 = 0`
//! `dy_t = -b*y_t dt + eta dW2_t`,    `y_0 = 0`
//! `dW1_t dW2_t = rho dt`
//! `r_t = x_t + y_t + phi0`
//!
//! Dos factores en vez de uno permite decorrelar el nivel de tipos a corto y largo plazo
//! (`rho` habitualmente negativo, PLAN.md §7.16) -- limitación conocida del `HullWhite1F` de
//! un único factor, ver `crate::models::hull_white`. Igual que allí, `x_t`/`y_t` y los
//! parámetros son tensores Burn vectorizados sobre paths (forma `[n_paths]` o `[1]`,
//! difundido), y la implementación de `crate::models::ShortRateModel` (mismo estado que
//! consume `crate::products::irs::IrSwap::npv`, solo que aquí `State` es un par de tensores en
//! vez de uno) es lo único que expone a la capa de producto -- el resto de esta interfaz
//! homogénea (PLAN.md §7.16) es privado a este módulo.

use crate::kernel::euler_maruyama_step;
use crate::models::ShortRateModel;
use burn::tensor::backend::Backend;
use burn::tensor::Tensor;

/// Parámetros del modelo, genéricos sobre `B: Backend` (mismo patrón que `HullWhite1F`):
/// instanciar con `B = Autodiff<CpuBackend>` y `r0.require_grad()` propaga sensibilidades a
/// través de la simulación y la valoración (PLAN.md §5.3) -- igual que en el modelo de 1
/// factor, solo `r0` necesita gradiente en la práctica (medida "DV01").
#[derive(Debug, Clone)]
pub struct HullWhite2F<B: Backend> {
    /// Velocidad de reversión a la media del primer factor `x_t`.
    pub a: Tensor<B, 1>,
    /// Velocidad de reversión a la media del segundo factor `y_t`.
    pub b: Tensor<B, 1>,
    /// Volatilidad del primer factor.
    pub sigma: Tensor<B, 1>,
    /// Volatilidad del segundo factor.
    pub eta: Tensor<B, 1>,
    /// Correlación instantánea entre los dos brownianos que mueven `x_t`/`y_t` (típicamente
    /// negativa en la práctica, PLAN.md §7.16). Escalar `f64` plano, no tensor: es un
    /// coeficiente adimensional que solo entra en la simulación (correlacionar shocks) y en
    /// la fórmula cerrada del bono, nunca hace falta diferenciarlo.
    pub rho: f64,
    /// Desplazamiento constante `phi0` -- con `x_0 = y_0 = 0`, coincide con el tipo corto
    /// inicial `r_0` (de ahí el nombre, mismo rol que `r0` en `HullWhite1F`).
    pub r0: Tensor<B, 1>,
}

impl<B: Backend> HullWhite2F<B> {
    pub fn new(a: Tensor<B, 1>, b: Tensor<B, 1>, sigma: Tensor<B, 1>, eta: Tensor<B, 1>, rho: f64, r0: Tensor<B, 1>) -> Self {
        Self { a, b, sigma, eta, rho, r0 }
    }

    /// `B(z,τ) = (1 - exp(-z*τ)) / z`, misma fórmula que `HullWhite1F::b_factor` aplicada a
    /// cada uno de los dos factores por separado.
    fn b_factor(z: &Tensor<B, 1>, tau: f64) -> Tensor<B, 1> {
        let neg_z_tau = z.clone().mul_scalar(-tau);
        let one_minus_exp = neg_z_tau.exp().neg().add_scalar(1.0);
        one_minus_exp.div(z.clone())
    }

    /// Término de varianza propia de un factor con reversión `z` y volatilidad `vol`
    /// (Brigo-Mercurio ec. 4.11): `vol^2/z^2 * [τ + (2/z)e^{-zτ} - (1/(2z))e^{-2zτ} - 3/(2z)]`.
    fn variance_term(z: &Tensor<B, 1>, vol: &Tensor<B, 1>, tau: f64) -> Tensor<B, 1> {
        let inv_z = z.clone().powf_scalar(-1.0);
        let e1 = z.clone().mul_scalar(-tau).exp();
        let e2 = z.clone().mul_scalar(-2.0 * tau).exp();
        let bracket = (inv_z.clone().mul_scalar(2.0) * e1
            - inv_z.clone().mul_scalar(0.5) * e2
            - inv_z.mul_scalar(1.5))
        .add_scalar(tau);
        (vol.clone() * vol.clone()).div(z.clone() * z.clone()) * bracket
    }

    /// Término cruzado de covarianza entre los dos factores (Brigo-Mercurio ec. 4.11):
    /// `2*rho*sigma*eta/(a*b) * [τ + (e^{-aτ}-1)/a + (e^{-bτ}-1)/b - (e^{-(a+b)τ}-1)/(a+b)]`.
    fn cross_term(&self, tau: f64) -> Tensor<B, 1> {
        let a = self.a.clone();
        let b = self.b.clone();
        let a_plus_b = a.clone() + b.clone();

        let term_a = (a.clone().mul_scalar(-tau).exp().add_scalar(-1.0)).div(a.clone());
        let term_b = (b.clone().mul_scalar(-tau).exp().add_scalar(-1.0)).div(b.clone());
        let term_ab = (a_plus_b.clone().mul_scalar(-tau).exp().add_scalar(-1.0)).div(a_plus_b);
        let bracket = (term_a.add_scalar(tau) + term_b) - term_ab;

        let coef = (self.sigma.clone() * self.eta.clone()).mul_scalar(2.0 * self.rho).div(a * b);
        coef * bracket
    }

    /// Precio analítico del bono cero-cupón `P(t,T)` dado el estado `(x_t, y_t)` observado en
    /// `t`, bajo la fórmula cerrada afín de G2++ con `phi(s) = phi0` constante (ver docs del
    /// módulo): `P(t,T) = exp(-phi0*(T-t) + 0.5*V(t,T) - B(a,τ)x_t - B(b,τ)y_t)`.
    pub fn zero_coupon_bond(&self, x_t: Tensor<B, 1>, y_t: Tensor<B, 1>, t: f64, maturity: f64) -> Tensor<B, 1> {
        let tau = maturity - t;
        let variance = Self::variance_term(&self.a, &self.sigma, tau)
            + Self::variance_term(&self.b, &self.eta, tau)
            + self.cross_term(tau);

        let exponent = self.r0.clone().mul_scalar(-tau) + variance.mul_scalar(0.5)
            - Self::b_factor(&self.a, tau) * x_t
            - Self::b_factor(&self.b, tau) * y_t;
        exponent.exp()
    }

    /// Simula, vectorizado sobre todos los paths a la vez, las trayectorias de los dos
    /// factores latentes por discretización de Euler-Maruyama (`crate::kernel::
    /// euler_maruyama_step`, reutilizado tal cual para cada factor -- mismo kernel que
    /// `HullWhite1F::simulate_path`). `shocks_x`/`shocks_y` son shocks normales estándar
    /// *independientes* (forma `[n_paths]` cada uno, mismo largo): la correlación `rho` se
    /// aplica aquí dentro, construyendo `dW2 = rho*dW1 + sqrt(1-rho^2)*shocks_y` (Cholesky de
    /// una matriz de correlación 2x2). Devuelve `shocks_x.len() + 1` pares `(x_t, y_t)`
    /// (incluyendo el estado inicial `(0, 0)`).
    pub fn simulate_path(
        &self,
        dt: f64,
        shocks_x: &[Tensor<B, 1>],
        shocks_y: &[Tensor<B, 1>],
    ) -> Vec<(Tensor<B, 1>, Tensor<B, 1>)> {
        assert_eq!(shocks_x.len(), shocks_y.len(), "shocks_x y shocks_y deben tener el mismo largo");
        let sqrt_one_minus_rho2 = (1.0 - self.rho * self.rho).sqrt();

        let mut x = shocks_x[0].clone().mul_scalar(0.0);
        let mut y = x.clone();
        let mut path = Vec::with_capacity(shocks_x.len() + 1);
        path.push((x.clone(), y.clone()));

        for (zx, zy_indep) in shocks_x.iter().zip(shocks_y.iter()) {
            let zy = zy_indep.clone().mul_scalar(sqrt_one_minus_rho2) + zx.clone().mul_scalar(self.rho);

            let drift_x = self.a.clone().neg() * x.clone();
            let drift_y = self.b.clone().neg() * y.clone();
            x = euler_maruyama_step(x, drift_x, self.sigma.clone(), dt, zx.clone());
            y = euler_maruyama_step(y, drift_y, self.eta.clone(), dt, zy);
            path.push((x.clone(), y.clone()));
        }
        path
    }
}

/// Implementación de la interfaz homogénea `ShortRateModel` (PLAN.md §7.16): el estado de
/// `HullWhite2F` es el par de factores latentes `(x_t, y_t)`, a diferencia del tipo corto
/// escalar de `HullWhite1F` -- `crate::products::irs::IrSwap` no necesita saber la diferencia.
impl<B: Backend> ShortRateModel<B> for HullWhite2F<B> {
    type State = (Tensor<B, 1>, Tensor<B, 1>);

    fn zero_coupon_bond(&self, state: Self::State, t: f64, maturity: f64) -> Tensor<B, 1> {
        HullWhite2F::zero_coupon_bond(self, state.0, state.1, t, maturity)
    }
}

/// Factor de descuento estocástico `exp(-∫_0^T r_s ds)` a lo largo de una trayectoria
/// simulada, con `r_s = x_s + y_s + phi0` (mismo rol que `hull_white::path_discount_factor`,
/// aproximando la integral por la regla del trapecio).
pub fn path_discount_factor<B: Backend>(
    path: &[(Tensor<B, 1>, Tensor<B, 1>)],
    phi0: Tensor<B, 1>,
    dt: f64,
) -> Tensor<B, 1> {
    let r: Vec<Tensor<B, 1>> = path
        .iter()
        .map(|(x, y)| x.clone() + y.clone() + phi0.clone())
        .collect();

    let mut integral = r[0].clone().mul_scalar(0.0);
    for window in r.windows(2) {
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

    /// Parámetros típicos de G2++ en la literatura (Brigo-Mercurio): `rho` negativo,
    /// volatilidades del orden de 1%, no calibrados a mercado real (mismo estatus que
    /// `hull_white::tests::reference_model`, solo un punto de referencia razonable).
    fn reference_model() -> HullWhite2F<CpuBackend> {
        HullWhite2F::new(
            scalar(0.1),
            scalar(0.2),
            scalar(0.01),
            scalar(0.012),
            -0.7,
            scalar(0.03),
        )
    }

    #[test]
    fn zero_coupon_bond_at_zero_maturity_is_one() {
        let model = reference_model();
        let p = model.zero_coupon_bond(scalar(0.01), scalar(-0.005), 1.0, 1.0);
        assert!((to_f64(p) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn zero_coupon_bond_is_decreasing_in_maturity() {
        let model = reference_model();
        let x0 = scalar(0.0);
        let y0 = scalar(0.0);
        let p1 = to_f64(model.zero_coupon_bond(x0.clone(), y0.clone(), 0.0, 1.0));
        let p5 = to_f64(model.zero_coupon_bond(x0.clone(), y0.clone(), 0.0, 5.0));
        let p10 = to_f64(model.zero_coupon_bond(x0, y0, 0.0, 10.0));
        assert!(p1 > p5 && p5 > p10, "p1={p1} p5={p5} p10={p10}");
    }

    #[test]
    fn zero_coupon_bond_matches_one_factor_when_second_factor_is_degenerate() {
        // Con b grande y eta=rho=0 el segundo factor se extingue casi al instante y nunca se
        // correlaciona con el primero: el precio de G2++ debe coincidir (dentro de tolerancia
        // numérica) con el de un Hull-White de 1 factor con los mismos a/sigma/r0 -- caso
        // límite que valida la fórmula cerrada de dos factores contra la ya verificada de uno.
        use crate::models::hull_white::HullWhite1F;

        let a = 0.1;
        let sigma = 0.01;
        let r0 = 0.03;
        let model_1f = HullWhite1F::new(scalar(a), scalar(r0), scalar(sigma));
        let model_2f = HullWhite2F::new(scalar(a), scalar(50.0), scalar(sigma), scalar(0.0), 0.0, scalar(r0));

        for maturity in [1.0, 5.0, 10.0] {
            let p_1f = to_f64(model_1f.zero_coupon_bond(scalar(r0), 0.0, maturity));
            let p_2f = to_f64(model_2f.zero_coupon_bond(scalar(0.0), scalar(0.0), 0.0, maturity));
            assert!(
                (p_1f - p_2f).abs() < 1e-6,
                "maturity={maturity} p_1f={p_1f} p_2f={p_2f}"
            );
        }
    }

    /// PLAN.md §5.6 capa 2 (extendida al segundo modelo, §7.16): convergencia Monte Carlo vs
    /// fórmula cerrada, mismo patrón que `hull_white::tests::monte_carlo_bond_price_converges_
    /// to_analytic_formula` pero simulando los dos factores correlacionados a la vez.
    #[test]
    fn monte_carlo_bond_price_converges_to_analytic_formula() {
        let model = reference_model();
        let phi0 = 0.03;
        let maturity = 5.0;
        let n_steps = 250;
        let n_paths = 50_000;
        let dt = maturity / n_steps as f64;
        let device = Device::default();

        let shocks_x: Vec<Tensor<CpuBackend, 1>> = (0..n_steps)
            .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), &device))
            .collect();
        let shocks_y: Vec<Tensor<CpuBackend, 1>> = (0..n_steps)
            .map(|_| Tensor::random([n_paths], Distribution::Normal(0.0, 1.0), &device))
            .collect();

        let path = model.simulate_path(dt, &shocks_x, &shocks_y);
        let discounts = path_discount_factor(&path, scalar(phi0), dt);

        let mc_price = to_f64(discounts.clone().mean());
        let variance = to_f64(
            (discounts.clone() - discounts.clone().mean())
                .powf_scalar(2.0)
                .mean(),
        );
        let std_error = (variance / n_paths as f64).sqrt();

        let analytic_price = to_f64(model.zero_coupon_bond(scalar(0.0), scalar(0.0), 0.0, maturity));

        let tolerance = 8.0 * std_error;
        assert!(
            (mc_price - analytic_price).abs() < tolerance,
            "MC={mc_price} analytic={analytic_price} tol={tolerance} (std_error={std_error})"
        );
    }
}
