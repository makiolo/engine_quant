//! Interest Rate Swap (IRS) vanilla — PLAN.md §5.2, caso base del prototipo.
//!
//! Valoración por réplica en bonos cero-cupón bajo curva única (misma curva de
//! descuento y de proyección de la pata flotante): la pata flotante de un swap que
//! resetea a mercado en cada periodo vale `P(t,T0) - P(t,Tn)` por unidad de nocional, y la
//! pata fija vale `sum_i tau_i * K * P(t,Ti)`. Esta réplica es exacta y no necesita
//! Monte Carlo para un único nodo de valoración — el Monte Carlo (ver
//! `crate::exposure`) se usa para simular el *estado futuro* (el tipo corto `r_t`) en
//! las fechas de monitorización de la exposición, revalorando el swap analíticamente en
//! cada una vía estas mismas fórmulas.
//!
//! **Limitación de esta primera versión**: la fórmula de la pata flotante asume que la
//! fecha de valoración `t` es anterior o igual a `start` (el swap aún no ha fijado su
//! primer cupón flotante). Valorar en mitad de un periodo de acumulación ya fijado
//! requiere llevar el último fixing como estado adicional — se deja para cuando haga
//! falta (Fase 2+), no es necesario para el perfil de exposición del prototipo.

use crate::models::hull_white::HullWhite1F;
use crate::scalar::Scalar;

/// IRS vanilla: nocional fijo, tipo fijo `K`, fechas de pago `T_1 < ... < T_n` con
/// fracciones de año (`accruals[i]` = `T_i - T_{i-1}`, con `T_0 = start`).
#[derive(Debug, Clone)]
pub struct IrSwap<T: Scalar> {
    pub notional: T,
    pub fixed_rate: T,
    /// Fecha de inicio de la pata flotante (`T_0`).
    pub start: f64,
    /// Fechas de pago `T_1..T_n` (absolutas, en años desde t=0).
    pub payment_times: Vec<f64>,
    /// Fracciones de año de cada periodo `(T_{i-1}, T_i]`, mismo largo que `payment_times`.
    pub accruals: Vec<f64>,
}

impl<T: Scalar> IrSwap<T> {
    /// NPV del swap pagador (paga fijo, recibe flotante) visto desde `t`, dado el tipo
    /// corto `r_t` observado en `t` y el modelo que descuenta/proyecta.
    ///
    /// Requiere `t <= self.start` (ver limitación documentada arriba).
    pub fn npv(&self, r_t: T, t: f64, model: &HullWhite1F<T>) -> T {
        debug_assert!(
            t <= self.start + 1e-9,
            "IrSwap::npv requiere t <= start en esta primera versión"
        );

        let p_start = model.zero_coupon_bond(r_t, t, self.start);
        let p_end = model.zero_coupon_bond(r_t, t, *self.payment_times.last().unwrap());
        let floating_leg = self.notional * (p_start - p_end);

        let fixed_leg: T = self
            .payment_times
            .iter()
            .zip(self.accruals.iter())
            .map(|(&ti, &tau)| {
                let p_i = model.zero_coupon_bond(r_t, t, ti);
                self.notional * self.fixed_rate * T::from_f64(tau) * p_i
            })
            .sum();

        floating_leg - fixed_leg
    }

    /// Tipo fijo a mercado (`NPV = 0`) visto desde `t = self.start`, dado `r_t` en esa
    /// fecha. Útil para construir swaps "a la par" en tests y en la fecha de arranque de
    /// un perfil de exposición.
    pub fn par_rate(
        notional: T,
        r_start: T,
        start: f64,
        payment_times: &[f64],
        accruals: &[f64],
        model: &HullWhite1F<T>,
    ) -> T {
        let p_start = model.zero_coupon_bond(r_start, start, start);
        let p_end = model.zero_coupon_bond(r_start, start, *payment_times.last().unwrap());
        let numerator = p_start - p_end;

        let denominator: T = payment_times
            .iter()
            .zip(accruals.iter())
            .map(|(&ti, &tau)| T::from_f64(tau) * model.zero_coupon_bond(r_start, start, ti))
            .sum();

        let _ = notional; // el nocional se cancela en el tipo a la par.
        numerator / denominator
    }

    /// `true` si `t` coincide (dentro de tolerancia numérica) con una fecha de reseteo
    /// válida del swap: `self.start` o cualquiera de sus fechas de pago salvo la última
    /// (la pata flotante no resetea de nuevo después del pago final).
    pub fn is_reset_date(&self, t: f64) -> bool {
        (t - self.start).abs() < 1e-9
            || self.payment_times[..self.payment_times.len() - 1]
                .iter()
                .any(|&ti| (ti - t).abs() < 1e-9)
    }

    /// Swap "restante" visto desde una fecha de reseteo `t` (`self.start` o una fecha de
    /// pago intermedia, ver [`Self::is_reset_date`]): mismo nocional y tipo fijo,
    /// conservando solo los periodos que empiezan en `t` o después. Permite valorar el
    /// swap en cualquier fecha de reseteo futura reutilizando [`Self::npv`], que asume
    /// que la pata flotante aún no ha fijado su próximo cupón — cierto por construcción
    /// para el swap restante, ya que `t` es precisamente su próxima fecha de reseteo.
    pub fn remaining_from(&self, t: f64) -> Self {
        debug_assert!(
            self.is_reset_date(t),
            "remaining_from requiere que t sea una fecha de reseteo del swap"
        );

        let mut payment_times = Vec::new();
        let mut accruals = Vec::new();
        let mut period_start = self.start;
        for (&ti, &tau) in self.payment_times.iter().zip(self.accruals.iter()) {
            if period_start >= t - 1e-9 {
                payment_times.push(ti);
                accruals.push(tau);
            }
            period_start = ti;
        }

        IrSwap {
            notional: self.notional,
            fixed_rate: self.fixed_rate,
            start: t,
            payment_times,
            accruals,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reference_model() -> HullWhite1F<f64> {
        HullWhite1F::new(0.1, 0.03, 0.01)
    }

    fn annual_5y_swap(fixed_rate: f64) -> IrSwap<f64> {
        IrSwap {
            notional: 1_000_000.0,
            fixed_rate,
            start: 0.0,
            payment_times: vec![1.0, 2.0, 3.0, 4.0, 5.0],
            accruals: vec![1.0, 1.0, 1.0, 1.0, 1.0],
        }
    }

    #[test]
    fn par_swap_has_zero_npv_at_start() {
        let model = reference_model();
        let r0 = 0.02;
        let swap = annual_5y_swap(0.0); // fixed_rate se sobreescribe abajo
        let k = IrSwap::par_rate(
            swap.notional,
            r0,
            swap.start,
            &swap.payment_times,
            &swap.accruals,
            &model,
        );
        let par_swap = annual_5y_swap(k);
        let npv = par_swap.npv(r0, 0.0, &model);
        assert!(
            npv.abs() < 1e-6,
            "NPV del swap a la par debería ser ~0, got {npv}"
        );
    }

    #[test]
    fn payer_swap_npv_increases_when_rates_rise() {
        let model = reference_model();
        let r0 = 0.02;
        let swap = annual_5y_swap(0.0);
        let k = IrSwap::par_rate(
            swap.notional,
            r0,
            swap.start,
            &swap.payment_times,
            &swap.accruals,
            &model,
        );
        let par_swap = annual_5y_swap(k);

        let npv_base = par_swap.npv(r0, 0.0, &model);
        let npv_higher_rate = par_swap.npv(r0 + 0.01, 0.0, &model);
        // Un swap pagador (paga fijo, recibe flotante) gana valor cuando suben los tipos.
        assert!(npv_higher_rate > npv_base);
    }

    #[test]
    fn remaining_from_drops_elapsed_periods() {
        let swap = annual_5y_swap(0.03);
        let remaining = swap.remaining_from(2.0);
        assert_eq!(remaining.start, 2.0);
        assert_eq!(remaining.payment_times, vec![3.0, 4.0, 5.0]);
        assert_eq!(remaining.accruals, vec![1.0, 1.0, 1.0]);
    }

    #[test]
    fn remaining_from_at_start_is_unchanged() {
        let swap = annual_5y_swap(0.03);
        let remaining = swap.remaining_from(0.0);
        assert_eq!(remaining.payment_times, swap.payment_times);
        assert_eq!(remaining.accruals, swap.accruals);
    }

    #[test]
    fn is_reset_date_recognizes_start_and_intermediate_payments_only() {
        let swap = annual_5y_swap(0.03);
        assert!(swap.is_reset_date(0.0));
        assert!(swap.is_reset_date(2.0));
        assert!(!swap.is_reset_date(5.0)); // pago final, no hay reseteo posterior
        assert!(!swap.is_reset_date(1.5));
    }
}
