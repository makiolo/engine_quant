//! Curva de tipos: la pieza que faltaba entre "un modelo con parámetros elegidos a mano"
//! (PLAN.md §5.2, `HullWhite1F` con `a`/`b`/`sigma` constantes) y "un modelo calibrado a algo
//! observable" (`crate::calibration`). Una `Curve` es solo datos — `pillars`/`zero_rates` en
//! `f64` puro, sin tipos de Burn — igual que `Params` no es una jerarquía polimórfica: si la
//! curva es "falsa" (fabricada para un test o una demo, `synthetic_from_hull_white`) o "real"
//! (tipos observados y bootstrapeados fuera de este crate) es una cuestión de *de dónde salen
//! los números*, no de un tipo distinto.
//!
//! **Renombrado desde `MarketSnapshot` (PLAN.md §7.20)**: este tipo nunca tuvo datos de
//! crédito (`hazard_rate`/`recovery_rate`) — esos solo existen en `engine::MarketSnapshot`
//! (la capa C++, PLAN.md §7.15), que compone una `Curve` (vía la C ABI/`cxx`, que sigue
//! hablando de pillars/zero_rates en `f64` puro) junto con esos dos campos. El nombre
//! `MarketSnapshot` aquí describía más de lo que el tipo hacía de verdad; `Curve` es exacto.
//!
//! **Simplificación deliberada de esta primera versión** (igual espíritu que la de
//! `crate::models::hull_white`): la curva ya viene como curva cero-cupón (`pillars` + tipos
//! cero continuos), no como los instrumentos de mercado crudos (depósitos, futuros, swaps) de
//! los que se bootstrapea una curva real — ese bootstrapping es trabajo futuro, no bloquea la
//! calibración de un modelo a una curva ya construida.

/// Curva de tipos observada (o fabricada) en un instante dado: pares `(pillar, zero_rate)`
/// paralelos, `pillars` en años desde hoy y estrictamente crecientes, `zero_rates` como tipos
/// cero de capitalización continua (`P(0,T) = exp(-zero_rate(T) * T)`).
#[derive(Debug, Clone, PartialEq)]
pub struct Curve {
    pillars: Vec<f64>,
    zero_rates: Vec<f64>,
}

impl Curve {
    /// Construye una curva a partir de pillars/zero_rates ya observados (curva "real") o
    /// fabricados a mano (curva "falsa"): esta función no distingue entre ambos casos, ver
    /// `synthetic_from_hull_white` para un atajo específico del segundo.
    ///
    /// Entra en pánico si los vectores no tienen el mismo largo, están vacíos, o `pillars` no
    /// es estrictamente creciente (`zero_rate`/`discount_factor` interpolan linealmente entre
    /// pillars consecutivos: sin este invariante la interpolación no tiene sentido).
    pub fn new(pillars: Vec<f64>, zero_rates: Vec<f64>) -> Self {
        assert_eq!(pillars.len(), zero_rates.len(), "pillars y zero_rates deben tener el mismo largo");
        assert!(!pillars.is_empty(), "Curve necesita al menos un pillar");
        for window in pillars.windows(2) {
            assert!(window[1] > window[0], "pillars debe ser estrictamente creciente");
        }
        Self { pillars, zero_rates }
    }

    /// Curva "falsa" (PLAN.md): fabrica una `Curve` leyendo la propia fórmula cerrada de
    /// `HullWhite1F` (`crate::smoke::hull_white_zero_coupon_bond`) en los pillars dados. Útil
    /// para probar/demostrar `crate::calibration` sin depender de datos de mercado reales:
    /// generar con unos parámetros conocidos, calibrar desde otra estimación inicial,
    /// comprobar que se recuperan.
    pub fn synthetic_from_hull_white(a: f64, b: f64, sigma: f64, r0: f64, pillars: Vec<f64>) -> Self {
        let zero_rates = pillars
            .iter()
            .map(|&t| {
                let price = crate::smoke::hull_white_zero_coupon_bond(a, b, sigma, r0, 0.0, t);
                if t > 0.0 {
                    -price.ln() / t
                } else {
                    0.0
                }
            })
            .collect();
        Self::new(pillars, zero_rates)
    }

    /// Curva "falsa" (PLAN.md §7.16), equivalente de dos factores de
    /// `synthetic_from_hull_white`: fabrica una `Curve` leyendo la propia fórmula cerrada de
    /// `HullWhite2F` (`crate::smoke::hull_white_2f_zero_coupon_bond`, factores latentes en su
    /// valor inicial) en los pillars dados.
    #[allow(clippy::too_many_arguments)]
    pub fn synthetic_from_hull_white_2f(a: f64, b: f64, sigma: f64, eta: f64, rho: f64, r0: f64, pillars: Vec<f64>) -> Self {
        let zero_rates = pillars
            .iter()
            .map(|&t| {
                let price = crate::smoke::hull_white_2f_zero_coupon_bond(a, b, sigma, eta, rho, r0, t);
                if t > 0.0 {
                    -price.ln() / t
                } else {
                    0.0
                }
            })
            .collect();
        Self::new(pillars, zero_rates)
    }

    pub fn pillars(&self) -> &[f64] {
        &self.pillars
    }

    pub fn zero_rates(&self) -> &[f64] {
        &self.zero_rates
    }

    /// Tipo cero continuo interpolado linealmente entre los dos pillars que rodean a `t`, con
    /// extrapolación plana (el valor del pillar más próximo) fuera de `[pillars[0],
    /// pillars[ultimo]]` — la convención más simple que no requiere asumir nada sobre la forma
    /// de la curva más allá de los pillars observados.
    pub fn zero_rate(&self, t: f64) -> f64 {
        if t <= self.pillars[0] {
            return self.zero_rates[0];
        }
        let last = self.pillars.len() - 1;
        if t >= self.pillars[last] {
            return self.zero_rates[last];
        }
        // self.pillars tiene al menos 2 elementos aquí (si tuviera 1, uno de los dos returns
        // de arriba ya habría disparado, ya que t <= pillars[0] o t >= pillars[last=0]).
        let i = self.pillars.partition_point(|&p| p <= t).max(1);
        let (t0, t1) = (self.pillars[i - 1], self.pillars[i]);
        let (z0, z1) = (self.zero_rates[i - 1], self.zero_rates[i]);
        let w = (t - t0) / (t1 - t0);
        z0 + w * (z1 - z0)
    }

    /// Factor de descuento `P(0,T)` implícito en `zero_rate(t)`.
    pub fn discount_factor(&self, t: f64) -> f64 {
        (-self.zero_rate(t) * t).exp()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_rate_interpolates_linearly_between_pillars() {
        let curve = Curve::new(vec![1.0, 2.0, 5.0], vec![0.02, 0.03, 0.04]);
        assert!((curve.zero_rate(1.0) - 0.02).abs() < 1e-12);
        assert!((curve.zero_rate(2.0) - 0.03).abs() < 1e-12);
        // A mitad de camino entre 2.0 (0.03) y 5.0 (0.04): interpolación lineal simple.
        assert!((curve.zero_rate(3.5) - 0.035).abs() < 1e-12);
    }

    #[test]
    fn zero_rate_extrapolates_flat_outside_pillars() {
        let curve = Curve::new(vec![1.0, 5.0], vec![0.02, 0.04]);
        assert!((curve.zero_rate(0.1) - 0.02).abs() < 1e-12);
        assert!((curve.zero_rate(10.0) - 0.04).abs() < 1e-12);
    }

    #[test]
    fn discount_factor_matches_continuous_compounding_formula() {
        let curve = Curve::new(vec![1.0, 2.0], vec![0.02, 0.02]);
        let expected = (-0.02_f64 * 2.0).exp();
        assert!((curve.discount_factor(2.0) - expected).abs() < 1e-12);
    }

    #[test]
    fn synthetic_from_hull_white_reproduces_the_models_own_prices() {
        let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
        let pillars = vec![1.0, 2.0, 5.0, 10.0];
        let curve = Curve::synthetic_from_hull_white(a, b, sigma, r0, pillars.clone());

        for &t in &pillars {
            let expected = crate::smoke::hull_white_zero_coupon_bond(a, b, sigma, r0, 0.0, t);
            assert!(
                (curve.discount_factor(t) - expected).abs() < 1e-9,
                "t={t}: discount_factor={} vs esperado={expected}",
                curve.discount_factor(t)
            );
        }
    }

    #[test]
    #[should_panic(expected = "estrictamente creciente")]
    fn new_rejects_non_increasing_pillars() {
        Curve::new(vec![1.0, 1.0], vec![0.02, 0.03]);
    }

    #[test]
    fn synthetic_from_hull_white_2f_reproduces_the_models_own_prices() {
        let (a, b, sigma, eta, rho, r0) = (0.1, 0.2, 0.01, 0.012, -0.7, 0.03);
        let pillars = vec![1.0, 2.0, 5.0, 10.0];
        let curve = Curve::synthetic_from_hull_white_2f(a, b, sigma, eta, rho, r0, pillars.clone());

        for &t in &pillars {
            let expected = crate::smoke::hull_white_2f_zero_coupon_bond(a, b, sigma, eta, rho, r0, t);
            assert!(
                (curve.discount_factor(t) - expected).abs() < 1e-9,
                "t={t}: discount_factor={} vs esperado={expected}",
                curve.discount_factor(t)
            );
        }
    }
}
