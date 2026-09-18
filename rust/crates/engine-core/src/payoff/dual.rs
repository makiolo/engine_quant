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

// En la implementación de Mul, las sumas y productos son exactamente la regla del
// producto para derivadas duales; no representan aritmética sospechosa del operador.
#![allow(clippy::suspicious_arithmetic_impl)]

use std::ops::{Add, Div, Mul, Neg, Sub};

/// Trait minimo de numero dual truncado (PLAN_HYPERDUAL.md §3.1/ADR-HD-01): exactamente las
/// operaciones que el interprete de payoff (`ScalarOp`, ver `super::sensitivity`) usa, ni una mas
/// -- `Add`/`Sub`/`Mul`/`Div`/`Neg` como supertraits (`ScalarOp` los cubre via `std::ops`) mas las
/// 6 funciones que `Dual` ya implementa a mano. Deliberadamente NO incluye un constructor
/// `variable()`/"seedear una direccion": cada tipo de la familia (`Dual`, `Dual2`, `HyperDual`)
/// tiene una nocion distinta de que significa esa direccion, y la decide `GbmDualPath` (o su
/// generalizacion), no el trait.
pub(crate) trait DualNumber:
    Copy + Add<Output = Self> + Sub<Output = Self> + Mul<Output = Self> + Div<Output = Self> + Neg<Output = Self>
{
    /// Valor base (`f64`), sin derivada -- equivalente generico del campo `.value` de `Dual`.
    fn re(self) -> f64;
    /// Constante: todas las componentes de derivada a cero (equivalente generico de `Dual::constant`).
    fn constant(value: f64) -> Self;

    fn abs(self) -> Self;
    fn exp(self) -> Self;
    fn ln(self) -> Self;
    fn powf(self, other: Self) -> Self;
    fn min(self, other: Self) -> Self;
    fn max(self, other: Self) -> Self;
}

/// Multiplicacion `f64` instrumentada (PLAN_HYPERDUAL.md §8.4): en builds de test cuenta cada
/// multiplicacion real realizada por `Mul` de `Dual`/`Dual2`/`HyperDual` en un contador por hilo,
/// para verificar la tabla de costes de §0.1 contra la aritmetica real en vez de solo calcularla a
/// mano. Fuera de `cfg(test)` es una multiplicacion `f64` lisa (se espera que el compilador la
/// inline por completo, cero coste extra en produccion).
#[inline(always)]
fn mul_f64(a: f64, b: f64) -> f64 {
    #[cfg(test)]
    mul_count::record();
    a * b
}

#[cfg(test)]
pub(crate) mod mul_count {
    use std::cell::Cell;

    thread_local! {
        static COUNT: Cell<usize> = const { Cell::new(0) };
    }

    /// Pone el contador a cero -- llamar antes de la operacion cuyo coste se quiere medir (cada
    /// `#[test]` corre en su propio hilo, asi que no hay interferencia entre tests).
    pub(crate) fn reset() {
        COUNT.with(|c| c.set(0));
    }

    /// Multiplicaciones `f64` registradas desde el ultimo `reset()`.
    pub(crate) fn get() -> usize {
        COUNT.with(|c| c.get())
    }

    pub(crate) fn record() {
        COUNT.with(|c| c.set(c.get() + 1));
    }
}

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
        Dual {
            value: mul_f64(self.value, rhs.value),
            deriv: mul_f64(self.deriv, rhs.value) + mul_f64(self.value, rhs.deriv),
        }
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

/// Impl mecanica (PLAN_HYPERDUAL.md Fase 1, §7): delega en los metodos inherentes que `Dual` ya
/// tiene, sin cambiar una linea de su comportamiento -- los call sites existentes (`Dual::exp()`,
/// `Dual::abs()`, ...) siguen resolviendo al metodo inherente, no a este impl de trait (Rust
/// prioriza metodos inherentes sobre metodos de trait en resolucion de metodo), asi que este impl
/// solo se ejerce cuando algo lo usa a traves de `T: DualNumber` generico (`super::sensitivity`).
impl DualNumber for Dual {
    fn re(self) -> f64 {
        self.value
    }
    fn constant(value: f64) -> Self {
        Dual::constant(value)
    }
    fn abs(self) -> Self {
        Dual::abs(self)
    }
    fn exp(self) -> Self {
        Dual::exp(self)
    }
    fn ln(self) -> Self {
        Dual::ln(self)
    }
    fn powf(self, other: Self) -> Self {
        Dual::powf(self, other)
    }
    fn min(self, other: Self) -> Self {
        Dual::min(self, other)
    }
    fn max(self, other: Self) -> Self {
        Dual::max(self, other)
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

    // PLAN_HYPERDUAL.md §8.4/§0.1: coste MEDIDO (no solo calculado a mano) de la multiplicacion --
    // confirma que la tabla de §0.1 no se degrada en silencio si alguien "optimiza" la aritmetica
    // de forma incorrecta.
    #[test]
    fn multiplication_cost_matches_the_documented_cauchy_product_table() {
        mul_count::reset();
        let _ = Dual::variable(1.3) * Dual::variable(2.1);
        assert_eq!(mul_count::get(), 3, "Dual: 3 multiplicaciones por Mul (PLAN_HYPERDUAL.md §0.1)");
    }
}
