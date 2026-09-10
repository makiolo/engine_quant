//! Modelos de mercado (PLAN.md §3.2, registrados desde la capa C++ en Fase 2).

pub mod hull_white;
pub mod hull_white_2f;

use burn::tensor::backend::Backend;
use burn::tensor::Tensor;

/// Interfaz homogénea que comparten todos los modelos de tipo corto (PLAN.md §7.16: segundo
/// modelo, Hull-White 2 factores, junto al `HullWhite1F` de §7.5): lo único que
/// `crate::products::irs::IrSwap` necesita de un modelo para valorarse es el precio del bono
/// cero-cupón dado el estado del modelo en `t` -- el resto (número de factores, forma del
/// estado, correlación entre ellos) es un detalle interno de cada modelo concreto, expresado
/// en el tipo asociado `State` (`Tensor<B, 1>` para `HullWhite1F`, un par de tensores para
/// `HullWhite2F`). Esta genericidad evita duplicar `IrSwap::npv`/`par_rate` por modelo.
pub trait ShortRateModel<B: Backend> {
    /// Estado del modelo en un instante `t` (el tipo corto para un modelo de 1 factor, el
    /// par de factores latentes para uno de 2 factores). `Clone` porque `IrSwap::npv` lo
    /// reutiliza para descontar cada pata del swap.
    type State: Clone;

    /// Precio `P(t,T)` del bono cero-cupón bajo el modelo, dado su estado en `t`.
    fn zero_coupon_bond(&self, state: Self::State, t: f64, maturity: f64) -> Tensor<B, 1>;
}
