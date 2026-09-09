//! Tipo escalar genérico sobre el que se parametrizan todos los kernels numéricos
//! (PLAN.md §5.3). Permite instanciar el mismo código de valoración tanto con `f64`
//! (valoración pura) como con un tipo diferenciable (AAD, ver `crate::dual::Dual`).
//!
//! Se define un trait propio en vez de depender de `num_traits::Float` completo:
//! solo exponemos las operaciones que los kernels realmente necesitan, lo que hace
//! mucho más barato implementar `Scalar` para un tipo dual/tape-based (no hay que
//! definir derivadas para `tan`, `atan`, etc. que el motor no usa).

use std::fmt::Debug;
use std::iter::Sum;
use std::ops::{Add, Div, Mul, Neg, Sub};

pub trait Scalar:
    Copy
    + Clone
    + Debug
    + Send
    + Sync
    + Sum
    + PartialOrd
    + Add<Output = Self>
    + Sub<Output = Self>
    + Mul<Output = Self>
    + Div<Output = Self>
    + Neg<Output = Self>
    + 'static
{
    /// Construye un escalar "constante" a partir de un `f64` (derivada nula si `Self`
    /// es un tipo diferenciable).
    fn from_f64(v: f64) -> Self;

    /// Proyección al valor primal en `f64`, para extraer el resultado final de un
    /// cálculo (o comparar contra una tolerancia numérica en tests).
    fn to_f64(self) -> f64;

    fn zero() -> Self {
        Self::from_f64(0.0)
    }

    fn one() -> Self {
        Self::from_f64(1.0)
    }

    fn exp(self) -> Self;
    fn ln(self) -> Self;
    fn sqrt(self) -> Self;
    fn powi(self, n: i32) -> Self;

    fn max(self, other: Self) -> Self;
    fn min(self, other: Self) -> Self;

    /// max(self, 0) — patrón recurrente en payoffs (ej. swaption, exposición positiva).
    fn max_zero(self) -> Self {
        self.max(Self::zero())
    }
}

impl Scalar for f64 {
    fn from_f64(v: f64) -> Self {
        v
    }

    fn to_f64(self) -> f64 {
        self
    }

    fn exp(self) -> Self {
        f64::exp(self)
    }

    fn ln(self) -> Self {
        f64::ln(self)
    }

    fn sqrt(self) -> Self {
        f64::sqrt(self)
    }

    fn powi(self, n: i32) -> Self {
        f64::powi(self, n)
    }

    fn max(self, other: Self) -> Self {
        f64::max(self, other)
    }

    fn min(self, other: Self) -> Self {
        f64::min(self, other)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn f64_scalar_basic_ops() {
        assert_eq!(f64::from_f64(3.0) + f64::from_f64(4.0), 7.0);
        assert_eq!((2.0_f64).powi(10), 1024.0);
        assert_eq!((0.0_f64).exp(), 1.0);
        assert_eq!((1.0_f64).ln(), 0.0);
        assert_eq!((4.0_f64).sqrt(), 2.0);
        assert_eq!((-1.0_f64).max_zero(), 0.0);
        assert_eq!((3.0_f64).max_zero(), 3.0);
    }
}
