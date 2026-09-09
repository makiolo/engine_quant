//! Diferenciación automática forward-mode de una sola variable (AAD, PLAN.md §5.3).
//!
//! `Dual` representa un número dual `val + eps * ε` con `ε^2 = 0`. Al propagar operaciones
//! aritméticas y funciones elementales a través de un cálculo, el campo `eps` acumula la
//! derivada del resultado respecto de la variable "activada" con `Dual::variable`, sin
//! necesidad de diferenciación numérica (bump-and-reval) ni de una tape en modo reverse.

use std::iter::Sum;
use std::ops::{Add, Div, Mul, Neg, Sub};

use crate::scalar::Scalar;

/// Número dual `val + eps * ε`, con `val` el valor primal y `eps` la derivada acumulada
/// respecto de la variable independiente activada vía `Dual::variable`.
#[derive(Debug, Clone, Copy)]
pub struct Dual {
    pub val: f64,
    pub eps: f64,
}

impl Dual {
    /// Constante: no depende de la variable de diferenciación, derivada 0.
    pub fn constant(val: f64) -> Self {
        Dual { val, eps: 0.0 }
    }

    /// Variable independiente: "activa" la diferenciación respecto de este valor,
    /// derivada 1 (d/dx x = 1).
    pub fn variable(val: f64) -> Self {
        Dual { val, eps: 1.0 }
    }

    /// Extrae la sensibilidad (derivada) acumulada al final de un cálculo.
    pub fn derivative(self) -> f64 {
        self.eps
    }
}

impl Add for Dual {
    type Output = Dual;
    fn add(self, rhs: Dual) -> Dual {
        Dual {
            val: self.val + rhs.val,
            eps: self.eps + rhs.eps,
        }
    }
}

impl Sub for Dual {
    type Output = Dual;
    fn sub(self, rhs: Dual) -> Dual {
        Dual {
            val: self.val - rhs.val,
            eps: self.eps - rhs.eps,
        }
    }
}

impl Mul for Dual {
    type Output = Dual;
    fn mul(self, rhs: Dual) -> Dual {
        // (a+bε)(c+dε) = ac + (ad+bc)ε  (ε² = 0, se descarta)
        Dual {
            val: self.val * rhs.val,
            eps: self.eps * rhs.val + self.val * rhs.eps,
        }
    }
}

impl Div for Dual {
    type Output = Dual;
    fn div(self, rhs: Dual) -> Dual {
        // (a+bε)/(c+dε) = a/c + (bc-ad)/c² ε
        Dual {
            val: self.val / rhs.val,
            eps: (self.eps * rhs.val - self.val * rhs.eps) / (rhs.val * rhs.val),
        }
    }
}

impl Neg for Dual {
    type Output = Dual;
    fn neg(self) -> Dual {
        Dual {
            val: -self.val,
            eps: -self.eps,
        }
    }
}

/// El orden de dos `Dual` se define comparando únicamente su parte primal (`val`);
/// la parte derivada (`eps`) se ignora a efectos de comparación/orden, igual que para
/// otros escalares numéricos donde solo el valor importa para max/min/ordenar.
impl PartialEq for Dual {
    fn eq(&self, other: &Self) -> bool {
        self.val == other.val
    }
}

impl PartialOrd for Dual {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        self.val.partial_cmp(&other.val)
    }
}

impl Sum<Dual> for Dual {
    fn sum<I: Iterator<Item = Dual>>(iter: I) -> Self {
        iter.fold(Dual::constant(0.0), |acc, x| acc + x)
    }
}

impl<'a> Sum<&'a Dual> for Dual {
    fn sum<I: Iterator<Item = &'a Dual>>(iter: I) -> Self {
        iter.fold(Dual::constant(0.0), |acc, x| acc + *x)
    }
}

impl Scalar for Dual {
    fn from_f64(v: f64) -> Self {
        Dual::constant(v)
    }

    fn to_f64(self) -> f64 {
        self.val
    }

    fn exp(self) -> Self {
        // d/dx exp(x) = exp(x)
        let e = self.val.exp();
        Dual {
            val: e,
            eps: self.eps * e,
        }
    }

    fn ln(self) -> Self {
        // d/dx ln(x) = 1/x
        Dual {
            val: self.val.ln(),
            eps: self.eps / self.val,
        }
    }

    fn sqrt(self) -> Self {
        // d/dx sqrt(x) = 1/(2*sqrt(x))
        let s = self.val.sqrt();
        Dual {
            val: s,
            eps: self.eps / (2.0 * s),
        }
    }

    fn powi(self, n: i32) -> Self {
        // d/dx x^n = n * x^(n-1)
        if n == 0 {
            Dual::constant(1.0)
        } else {
            Dual {
                val: self.val.powi(n),
                eps: self.eps * (n as f64) * self.val.powi(n - 1),
            }
        }
    }

    fn max(self, other: Self) -> Self {
        // Subgradiente estándar: se propaga la derivada del operando con mayor valor
        // primal; en caso de empate se elige `self` (convención arbitraria pero
        // determinista, análoga a f64::max).
        if self.val >= other.val {
            self
        } else {
            other
        }
    }

    fn min(self, other: Self) -> Self {
        // Simétrico a `max`: en caso de empate se elige `self`.
        if self.val <= other.val {
            self
        } else {
            other
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn power_x_squared() {
        // d/dx (x^2) en x=3 -> valor 9, derivada 6
        let x = Dual::variable(3.0);
        let y = x.powi(2);
        assert!((y.val - 9.0).abs() < 1e-9);
        assert!((y.derivative() - 6.0).abs() < 1e-9);
    }

    #[test]
    fn power_x_cubed() {
        // d/dx (x^3) en x=2 -> valor 8, derivada 12
        let x = Dual::variable(2.0);
        let y = x.powi(3);
        assert!((y.val - 8.0).abs() < 1e-9);
        assert!((y.derivative() - 12.0).abs() < 1e-9);
    }

    #[test]
    fn exp_at_zero() {
        // d/dx exp(x) en x=0 -> valor 1, derivada 1
        let x = Dual::variable(0.0);
        let y = x.exp();
        assert!((y.val - 1.0).abs() < 1e-9);
        assert!((y.derivative() - 1.0).abs() < 1e-9);
    }

    #[test]
    fn ln_at_two() {
        // d/dx ln(x) en x=2 -> derivada 0.5
        let x = Dual::variable(2.0);
        let y = x.ln();
        assert!((y.derivative() - 0.5).abs() < 1e-9);
    }

    #[test]
    fn sqrt_at_four() {
        // d/dx sqrt(x) en x=4 -> valor 2, derivada 0.25
        let x = Dual::variable(4.0);
        let y = x.sqrt();
        assert!((y.val - 2.0).abs() < 1e-9);
        assert!((y.derivative() - 0.25).abs() < 1e-9);
    }

    #[test]
    fn quotient_rule() {
        // d/dx (x / (x+1)) en x=2 -> derivada = 1/(x+1)^2 = 1/9
        let x = Dual::variable(2.0);
        let y = x / (x + Dual::constant(1.0));
        assert!((y.derivative() - (1.0 / 9.0)).abs() < 1e-9);
    }

    #[test]
    fn product_rule_with_exp() {
        // d/dx (x^2 * exp(x)) en x=1 -> derivada = (2x + x^2) * exp(x) = 3e
        let x = Dual::variable(1.0);
        let y = x.powi(2) * x.exp();
        let expected = 3.0 * std::f64::consts::E;
        assert!((y.derivative() - expected).abs() < 1e-9);
    }

    #[test]
    fn max_zero_negative_branch_wins_constant() {
        // max_zero(-2) = max(-2, 0) -> gana la constante 0, derivada 0
        let x = Dual::variable(-2.0);
        let y = x.max_zero();
        assert!((y.val - 0.0).abs() < 1e-9);
        assert!((y.derivative() - 0.0).abs() < 1e-9);
    }

    #[test]
    fn max_zero_positive_branch_wins_variable() {
        // max_zero(3) = max(3, 0) -> gana la variable, derivada 1
        let x = Dual::variable(3.0);
        let y = x.max_zero();
        assert!((y.val - 3.0).abs() < 1e-9);
        assert!((y.derivative() - 1.0).abs() < 1e-9);
    }
}
