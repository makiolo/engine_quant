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

use crate::models::ShortRateModel;
use burn::tensor::backend::Backend;
use burn::tensor::Tensor;

/// IRS vanilla: nocional fijo, tipo fijo `K`, fechas de pago `T_1 < ... < T_n` con
/// fracciones de año (`accruals[i]` = `T_i - T_{i-1}`, con `T_0 = start`). `notional` y
/// `fixed_rate` son tensores forma `[1]` (Burn los difunde contra `[n_paths]` al operar
/// con `r_t`), genéricos sobre `B: Backend` para poder diferenciarlos vía AAD (PLAN.md
/// §5.3) igual que `HullWhite1F`.
#[derive(Debug, Clone)]
pub struct IrSwap<B: Backend> {
    pub notional: Tensor<B, 1>,
    pub fixed_rate: Tensor<B, 1>,
    /// Fecha de inicio de la pata flotante (`T_0`).
    pub start: f64,
    /// Fechas de pago `T_1..T_n` (absolutas, en años desde t=0).
    pub payment_times: Vec<f64>,
    /// Fracciones de año de cada periodo `(T_{i-1}, T_i]`, mismo largo que `payment_times`.
    pub accruals: Vec<f64>,
}

impl<B: Backend> IrSwap<B> {
    /// NPV del swap pagador (paga fijo, recibe flotante) visto desde `t`, dado el estado del
    /// modelo en `t` (`state`, PLAN.md §7.16: el tipo corto para `HullWhite1F`, el par de
    /// factores latentes para `HullWhite2F` -- lo único que esta función necesita de `model`
    /// es descontar vía `ShortRateModel::zero_coupon_bond`, genérico sobre cuál sea).
    ///
    /// Requiere `t <= self.start` (ver limitación documentada arriba).
    pub fn npv<M: ShortRateModel<B>>(&self, state: M::State, t: f64, model: &M) -> Tensor<B, 1> {
        debug_assert!(
            t <= self.start + 1e-9,
            "IrSwap::npv requiere t <= start en esta primera versión"
        );

        let p_start = model.zero_coupon_bond(state.clone(), t, self.start);
        let p_end = model.zero_coupon_bond(state.clone(), t, *self.payment_times.last().unwrap());
        let floating_leg = self.notional.clone() * (p_start - p_end);

        let fixed_leg = self
            .payment_times
            .iter()
            .zip(self.accruals.iter())
            .map(|(&ti, &tau)| {
                let p_i = model.zero_coupon_bond(state.clone(), t, ti);
                self.notional.clone() * self.fixed_rate.clone() * p_i.mul_scalar(tau)
            })
            .reduce(|acc, leg| acc + leg)
            .expect("un swap necesita al menos un periodo");

        floating_leg - fixed_leg
    }

    /// Tipo fijo a mercado (`NPV = 0`) visto desde `t = self.start`, dado el estado del
    /// modelo en esa fecha. Útil para construir swaps "a la par" en tests y en la fecha de
    /// arranque de un perfil de exposición.
    pub fn par_rate<M: ShortRateModel<B>>(
        state: M::State,
        start: f64,
        payment_times: &[f64],
        accruals: &[f64],
        model: &M,
    ) -> Tensor<B, 1> {
        let p_start = model.zero_coupon_bond(state.clone(), start, start);
        let p_end = model.zero_coupon_bond(state.clone(), start, *payment_times.last().unwrap());
        let numerator = p_start - p_end;

        let denominator = payment_times
            .iter()
            .zip(accruals.iter())
            .map(|(&ti, &tau)| model.zero_coupon_bond(state.clone(), start, ti).mul_scalar(tau))
            .reduce(|acc, leg| acc + leg)
            .expect("un swap necesita al menos un periodo");

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
            notional: self.notional.clone(),
            fixed_rate: self.fixed_rate.clone(),
            start: t,
            payment_times,
            accruals,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::CpuBackend;
    use crate::models::hull_white::HullWhite1F;
    use burn::tensor::TensorData;

    type Device = burn::tensor::Device<CpuBackend>;

    fn scalar(value: f64) -> Tensor<CpuBackend, 1> {
        Tensor::from_data(TensorData::from([value]), &Device::default())
    }

    fn to_f64(t: Tensor<CpuBackend, 1>) -> f64 {
        t.into_data().to_vec::<f64>().unwrap()[0]
    }

    fn reference_model() -> HullWhite1F<CpuBackend> {
        HullWhite1F::new(scalar(0.1), scalar(0.03), scalar(0.01))
    }

    fn annual_5y_swap(fixed_rate: f64) -> IrSwap<CpuBackend> {
        IrSwap {
            notional: scalar(1_000_000.0),
            fixed_rate: scalar(fixed_rate),
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
        let k = IrSwap::par_rate(scalar(r0), swap.start, &swap.payment_times, &swap.accruals, &model);
        let par_swap = IrSwap {
            fixed_rate: k,
            ..swap
        };
        let npv = to_f64(par_swap.npv(scalar(r0), 0.0, &model));
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
        let k = IrSwap::par_rate(scalar(r0), swap.start, &swap.payment_times, &swap.accruals, &model);
        let par_swap = IrSwap {
            fixed_rate: k,
            ..swap
        };

        let npv_base = to_f64(par_swap.npv(scalar(r0), 0.0, &model));
        let npv_higher_rate = to_f64(par_swap.npv(scalar(r0 + 0.01), 0.0, &model));
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

    /// Interfaz homogénea (PLAN.md §7.16): exactamente el mismo `IrSwap::npv`/`par_rate` que
    /// las pruebas de arriba usan con `HullWhite1F`, aquí instanciado con `HullWhite2F` -- sin
    /// tocar una sola línea de `IrSwap`, solo cambia `M`/`state` (un par de tensores en vez de
    /// uno) y el modelo concreto.
    #[test]
    fn par_swap_has_zero_npv_at_start_under_hull_white_2f() {
        use crate::models::hull_white_2f::HullWhite2F;

        let model = HullWhite2F::new(scalar(0.1), scalar(0.2), scalar(0.01), scalar(0.012), -0.7, scalar(0.03));
        let state = (scalar(0.0), scalar(0.0));
        let swap = annual_5y_swap(0.0);

        let k = IrSwap::par_rate(state.clone(), swap.start, &swap.payment_times, &swap.accruals, &model);
        let par_swap = IrSwap { fixed_rate: k, ..swap };
        let npv = to_f64(par_swap.npv(state, 0.0, &model));
        assert!(
            npv.abs() < 1e-6,
            "NPV del swap a la par debería ser ~0, got {npv}"
        );
    }
}
