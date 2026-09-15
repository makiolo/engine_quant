//! Numero dual minimo (`valor` + `derivada`) para diferenciacion hacia adelante de una unica
//! direccion (PLAN_PRODUCTS.md §12 Fase 11, "AAD cuando el backend lo permita"). Deliberadamente
//! NO es el `Autodiff<CpuBackend>` de Burn que ya usa el repo para IRS/Hull-White
//! (`tests/aad_vs_bump_reval.rs`): ese mecanismo diferencia un grafo de TENSORES, y el interprete
//! pathwise de payoff (`eval::eval_scalar`/`eval_contract`) opera sobre `f64` escalares sueltos
//! con ramificacion de control (`If`/`Trigger`/`Exercise`) que nunca pasa por un tensor -- envolver
//! ese interprete en tensores de Burn solo para diferenciarlo sería una reescritura mucho mayor
//! que este numero dual, que basta para la regla de la cadena de la aritmetica escalar que
//! `ScalarOp` realmente usa (ver `super::sensitivity`).
//!
//! `deriv` es la derivada de `value` respecto de UN UNICO parametro escalar elegido por quien
//! construye el `Dual` inicial (`variable`) -- modo "forward" de un solo hilo, suficiente porque
//! cada llamada a `payoff::api::payoff_sensitivity_gbm_q` pide una sensibilidad a la vez.

use std::ops::{Add, Div, Mul, Neg, Sub};

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct Dual {
    pub(crate) value: f64,
    pub(crate) deriv: f64,
}

impl Dual {
    /// Constante: no depende del parametro que se esta diferenciando (`deriv = 0`).
    pub(crate) fn constant(value: f64) -> Self {
        Self { value, deriv: 0.0 }
    }

    /// El parametro respecto del cual se deriva (`deriv = 1`, punto de partida de la regla de la
    /// cadena).
    pub(crate) fn variable(value: f64) -> Self {
        Self { value, deriv: 1.0 }
    }

    /// Subgradiente en `value == 0`: se toma el signo de `self.value >= 0.0`, igual que el
    /// `f64::abs` que reemplaza -- el conjunto donde el pathwise method y la derivada real
    /// discreparian (un kink exacto) tiene medida cero sobre las rutas simuladas.
    pub(crate) fn abs(self) -> Self {
        if self.value >= 0.0 {
            self
        } else {
            Self { value: -self.value, deriv: -self.deriv }
        }
    }

    pub(crate) fn exp(self) -> Self {
        let v = self.value.exp();
        Self { value: v, deriv: self.deriv * v }
    }

    pub(crate) fn ln(self) -> Self {
        Self { value: self.value.ln(), deriv: self.deriv / self.value }
    }

    /// `d(a^b)/dparam = a^b * (b' * ln(a) + b * a'/a)` para `a > 0` (formula general de
    /// exponenciacion con base y exponente ambos dependientes del parametro); si `a <= 0` y el
    /// exponente es constante (`other.deriv == 0`, el caso normal de `ScalarOp::Pow` con potencia
    /// entera/literal) cae a la regla de potencia simple `b * a^(b-1) * a'`, valida tambien para
    /// `a` negativo con `b` entero.
    pub(crate) fn powf(self, other: Self) -> Self {
        let v = self.value.powf(other.value);
        let deriv = if self.value > 0.0 {
            v * (other.deriv * self.value.ln() + other.value * self.deriv / self.value)
        } else if other.deriv == 0.0 {
            other.value * self.value.powf(other.value - 1.0) * self.deriv
        } else {
            f64::NAN
        };
        Self { value: v, deriv }
    }

    /// Igual que `Min` de `ScalarOp`: subgradiente que toma la rama de menor valor entera (con su
    /// propia derivada) -- coherente con `abs`/`max` en el empate.
    pub(crate) fn min(self, other: Self) -> Self {
        if self.value <= other.value {
            self
        } else {
            other
        }
    }

    pub(crate) fn max(self, other: Self) -> Self {
        if self.value >= other.value {
            self
        } else {
            other
        }
    }
}

impl Add for Dual {
    type Output = Dual;
    fn add(self, rhs: Dual) -> Dual {
        Dual { value: self.value + rhs.value, deriv: self.deriv + rhs.deriv }
    }
}

impl Sub for Dual {
    type Output = Dual;
    fn sub(self, rhs: Dual) -> Dual {
        Dual { value: self.value - rhs.value, deriv: self.deriv - rhs.deriv }
    }
}

impl Mul for Dual {
    type Output = Dual;
    fn mul(self, rhs: Dual) -> Dual {
        Dual { value: self.value * rhs.value, deriv: self.deriv * rhs.value + self.value * rhs.deriv }
    }
}

impl Div for Dual {
    type Output = Dual;
    fn div(self, rhs: Dual) -> Dual {
        Dual {
            value: self.value / rhs.value,
            deriv: (self.deriv * rhs.value - self.value * rhs.deriv) / (rhs.value * rhs.value),
        }
    }
}

impl Neg for Dual {
    type Output = Dual;
    fn neg(self) -> Dual {
        Dual { value: -self.value, deriv: -self.deriv }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const H: f64 = 1e-6;

    fn central_diff<F: Fn(f64) -> f64>(f: F, x: f64) -> f64 {
        (f(x + H) - f(x - H)) / (2.0 * H)
    }

    #[test]
    fn chain_rule_matches_finite_differences_for_a_representative_expression() {
        // f(x) = exp(x) * (x - 3).abs().powf(2.0) / (x + 5.0), en un punto donde ninguna rama
        // toca un kink (x != 3, x != -5) -- ejercita Mul/Sub/Div/Abs/Exp/Pow encadenados.
        let f = |x: f64| x.exp() * (x - 3.0).abs().powf(2.0) / (x + 5.0);
        let x0 = 1.7;

        let xd = Dual::variable(x0);
        let result = xd.exp() * (xd - Dual::constant(3.0)).abs().powf(Dual::constant(2.0))
            / (xd + Dual::constant(5.0));

        assert!((result.value - f(x0)).abs() < 1e-12, "value={} expected={}", result.value, f(x0));
        let expected_deriv = central_diff(f, x0);
        assert!(
            (result.deriv - expected_deriv).abs() < 1e-6,
            "deriv={} expected={}",
            result.deriv,
            expected_deriv
        );
    }

    #[test]
    fn min_and_max_take_the_derivative_of_the_active_branch() {
        let a = Dual::variable(2.0); // deriv 1 respecto de "a"
        let b = Dual::constant(5.0); // deriv 0 respecto de "a"
        assert_eq!(a.min(b), a);
        assert_eq!(a.max(b), b);
    }

    #[test]
    fn constant_has_zero_derivative_and_variable_has_unit_derivative() {
        assert_eq!(Dual::constant(3.0).deriv, 0.0);
        assert_eq!(Dual::variable(3.0).deriv, 1.0);
    }
}
