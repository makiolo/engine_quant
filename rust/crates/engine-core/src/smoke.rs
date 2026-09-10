//! Funciones de conveniencia en `f64` puro (sin tipos de Burn en la firma) para que
//! `engine-ffi` las bridgee sin depender directamente de Burn — mantiene la frontera cxx
//! aislada (PLAN.md §7: "`engine-ffi` como frontera aislada... si el mecanismo de FFI
//! cambiara, el impacto queda contenido a este crate"; lo mismo aplica a la librería de
//! cómputo interna).
//!
//! Sirven de cadena de humo ampliada (PLAN.md §7.1) que ejercita Hull-White + IRS +
//! exposición/CVA + AAD, ya sobre Burn (PLAN.md §5.1, §5.3), a través de las cuatro capas
//! del pipeline (Rust -> cxx -> C++ -> nanobind -> Python). No son la API definitiva del
//! motor — eso es el registry de Fase 2 (§5.4) — solo confirman que el pipeline completo
//! sigue funcionando de punta a punta con lógica de negocio real detrás, no solo `ping()`.

use crate::backend::{Autodiff, CpuBackend};
use crate::exposure::{expected_exposure_profile, unilateral_cva};
use crate::models::hull_white::HullWhite1F;
use crate::products::irs::IrSwap;
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

type Device = burn::tensor::Device<CpuBackend>;

fn scalar<B: Backend>(value: f64, device: &B::Device) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Precio del bono cero-cupón de Hull-White 1F (PLAN.md §5.2), vía el backend CPU de Burn.
pub fn hull_white_zero_coupon_bond(a: f64, b: f64, sigma: f64, r0: f64, t: f64, maturity: f64) -> f64 {
    let device = Device::default();
    let model: HullWhite1F<CpuBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    model.zero_coupon_bond(scalar(r0, &device), t, maturity).into_scalar()
}

/// Delta (`dP/dr0`) del bono cero-cupón, vía el autodiff en modo reverse de Burn (PLAN.md
/// §5.3): ejercita `Autodiff<CpuBackend>`, no solo la valoración pura.
pub fn hull_white_zero_coupon_bond_delta_r0(a: f64, b: f64, sigma: f64, r0: f64, t: f64, maturity: f64) -> f64 {
    type ADBackend = Autodiff<CpuBackend>;
    let device = Device::default();

    let model: HullWhite1F<ADBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let r0_var: Tensor<ADBackend, 1> = scalar(r0, &device).require_grad();
    let price = model.zero_coupon_bond(r0_var.clone(), t, maturity);
    let grads = price.backward();
    r0_var.grad(&grads).unwrap().into_scalar()
}

/// CVA unilateral de un IRS pagador de 5 años (cupones anuales, a la par) bajo Hull-White
/// 1F, vía el perfil de exposición Monte Carlo (PLAN.md §5.2): ejercita la simulación
/// vectorizada sobre paths (`Tensor::random` + backend CPU de Burn) de punta a punta, no
/// solo una fórmula cerrada. `seed` fija hace el resultado reproducible.
pub fn irs_unilateral_cva_5y(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    hazard_rate: f64,
    recovery_rate: f64,
    n_paths: u64,
    seed: u64,
) -> f64 {
    let device = Device::default();
    let model: HullWhite1F<CpuBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));

    let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
    let accruals = vec![1.0; 5];
    let fixed_rate = IrSwap::par_rate(scalar(r0, &device), 0.0, &payment_times, &accruals, &model);
    let swap = IrSwap {
        notional: scalar(notional, &device),
        fixed_rate,
        start: 0.0,
        payment_times,
        accruals,
    };

    let monitoring_times = vec![0.0, 1.0, 2.0, 3.0, 4.0];
    let profile =
        expected_exposure_profile(&model, &swap, r0, &monitoring_times, n_paths as usize, seed, &device);
    unilateral_cva(&profile, &model, r0, hazard_rate, recovery_rate, &device)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_coupon_bond_smoke_is_between_zero_and_one() {
        let p = hull_white_zero_coupon_bond(0.1, 0.03, 0.01, 0.02, 0.0, 5.0);
        assert!(p > 0.0 && p < 1.0, "P(0,5)={p} fuera de (0,1)");
    }

    #[test]
    fn zero_coupon_bond_delta_smoke_is_negative() {
        // Subir el tipo corto baja el precio del bono: dP/dr0 < 0.
        let delta = hull_white_zero_coupon_bond_delta_r0(0.1, 0.03, 0.01, 0.02, 0.0, 5.0);
        assert!(delta < 0.0, "delta={delta} debería ser negativo");
    }

    #[test]
    fn irs_unilateral_cva_smoke_is_nonnegative() {
        let cva = irs_unilateral_cva_5y(0.1, 0.03, 0.01, 0.02, 1_000_000.0, 0.02, 0.4, 2_000, 42);
        assert!(cva >= 0.0, "CVA={cva} debería ser >= 0");
    }
}
