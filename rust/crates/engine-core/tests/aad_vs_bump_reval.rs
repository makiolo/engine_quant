//! PLAN.md §5.6 capa 3: toda sensibilidad calculada vía AAD se contrasta contra su
//! equivalente por diferencias finitas (bump-and-reval), dentro de una tolerancia acorde
//! al step de bump usado. Vive como test de integración (no dentro de un módulo) porque
//! ejercita `backend`, `models::hull_white` e `products::irs` juntos — es la validación
//! de que delegar la AAD en el autodiff en modo reverse de Burn (PLAN.md §5.3) es
//! correcto, no solo que cada pieza compila.
//!
//! El mecanismo es literalmente el mismo código de valoración instanciado dos veces: una
//! con `CpuBackend` a secas (valoración pura, usada para el bump-and-reval) y otra con
//! `Autodiff<CpuBackend>` marcando el parámetro de interés con `.require_grad()` — no hay
//! un tipo `Dual` propio que mantener, Burn ya resuelve la diferenciación.

use burn::backend::Autodiff;
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};
use engine_core::backend::CpuBackend;
use engine_core::models::hull_white::HullWhite1F;
use engine_core::products::irs::IrSwap;

type ADBackend = Autodiff<CpuBackend>;
type Device = burn::tensor::Device<CpuBackend>;

fn scalar<B: Backend>(value: f64, device: &B::Device) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

fn to_f64<B: Backend>(t: Tensor<B, 1>) -> f64 {
    t.into_data().to_vec::<f64>().unwrap()[0]
}

/// Diferencia central de segundo orden: error de truncamiento `O(h^2)`.
fn central_diff<F: Fn(f64) -> f64>(f: F, x: f64, h: f64) -> f64 {
    (f(x + h) - f(x - h)) / (2.0 * h)
}

const BUMP_H: f64 = 1e-6;
// Con h=1e-6 el error de truncamiento de la diferencia central es O(h^2)=1e-12 y el de
// redondeo de punto flotante ~1e-16/h=1e-10; 1e-6 de tolerancia absoluta deja margen
// generoso para la curvatura real de estas funciones sin ocultar un error de signo o de
// regla de la cadena en la propagación de gradientes.
const TOLERANCE: f64 = 1e-6;

#[test]
fn aad_delta_of_zero_coupon_bond_wrt_r0_matches_bump_and_reval() {
    let (a, b, sigma, r0, t, maturity) = (0.1, 0.03, 0.01, 0.02, 0.0, 5.0);
    let device = Device::default();

    let model_ad: HullWhite1F<ADBackend> = HullWhite1F::new(
        scalar(a, &device),
        scalar(b, &device),
        scalar(sigma, &device),
    );
    let r0_var: Tensor<ADBackend, 1> = scalar(r0, &device).require_grad();
    let price = model_ad.zero_coupon_bond(r0_var.clone(), t, maturity);
    let grads = price.backward();
    let aad_delta = to_f64(r0_var.grad(&grads).unwrap());

    let model_f64: HullWhite1F<CpuBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let bump_delta = central_diff(
        |r| to_f64(model_f64.zero_coupon_bond(scalar(r, &device), t, maturity)),
        r0,
        BUMP_H,
    );

    assert!(
        (aad_delta - bump_delta).abs() < TOLERANCE,
        "delta AAD={aad_delta} vs bump-and-reval={bump_delta}"
    );
}

#[test]
fn aad_vega_of_zero_coupon_bond_wrt_sigma_matches_bump_and_reval() {
    let (a, b, sigma, r0, t, maturity) = (0.1, 0.03, 0.01, 0.02, 0.0, 5.0);
    let device = Device::default();

    let sigma_var: Tensor<ADBackend, 1> = scalar(sigma, &device).require_grad();
    let model_ad: HullWhite1F<ADBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), sigma_var.clone());
    let price = model_ad.zero_coupon_bond(scalar(r0, &device), t, maturity);
    let grads = price.backward();
    let aad_vega = to_f64(sigma_var.grad(&grads).unwrap());

    let bump_vega = central_diff(
        |s| {
            to_f64(
                HullWhite1F::<CpuBackend>::new(scalar(a, &device), scalar(b, &device), scalar(s, &device))
                    .zero_coupon_bond(scalar(r0, &device), t, maturity),
            )
        },
        sigma,
        BUMP_H,
    );

    assert!(
        (aad_vega - bump_vega).abs() < TOLERANCE,
        "vega AAD={aad_vega} vs bump-and-reval={bump_vega}"
    );
}

#[test]
fn aad_rho_of_zero_coupon_bond_wrt_mean_reversion_matches_bump_and_reval() {
    let (a, b, sigma, r0, t, maturity) = (0.1, 0.03, 0.01, 0.02, 0.0, 5.0);
    let device = Device::default();

    let a_var: Tensor<ADBackend, 1> = scalar(a, &device).require_grad();
    let model_ad: HullWhite1F<ADBackend> =
        HullWhite1F::new(a_var.clone(), scalar(b, &device), scalar(sigma, &device));
    let price = model_ad.zero_coupon_bond(scalar(r0, &device), t, maturity);
    let grads = price.backward();
    let aad_sens = to_f64(a_var.grad(&grads).unwrap());

    let bump_sens = central_diff(
        |a_bumped| {
            to_f64(
                HullWhite1F::<CpuBackend>::new(scalar(a_bumped, &device), scalar(b, &device), scalar(sigma, &device))
                    .zero_coupon_bond(scalar(r0, &device), t, maturity),
            )
        },
        a,
        BUMP_H,
    );

    assert!(
        (aad_sens - bump_sens).abs() < TOLERANCE,
        "sensibilidad a `a` AAD={aad_sens} vs bump-and-reval={bump_sens}"
    );
}

#[test]
fn aad_delta_of_irs_npv_wrt_r0_matches_bump_and_reval() {
    let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
    let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
    let accruals = vec![1.0, 1.0, 1.0, 1.0, 1.0];
    let notional = 1_000_000.0;
    let device = Device::default();

    let model_f64: HullWhite1F<CpuBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let fixed_rate_tensor =
        IrSwap::par_rate(scalar(r0, &device), 0.0, &payment_times, &accruals, &model_f64);
    let fixed_rate = to_f64(fixed_rate_tensor.clone());

    let swap_f64 = IrSwap {
        notional: scalar(notional, &device),
        fixed_rate: fixed_rate_tensor,
        start: 0.0,
        payment_times: payment_times.clone(),
        accruals: accruals.clone(),
    };

    let model_ad: HullWhite1F<ADBackend> =
        HullWhite1F::new(scalar(a, &device), scalar(b, &device), scalar(sigma, &device));
    let swap_ad = IrSwap {
        notional: scalar(notional, &device),
        fixed_rate: scalar(fixed_rate, &device),
        start: 0.0,
        payment_times: payment_times.clone(),
        accruals: accruals.clone(),
    };

    let r0_var: Tensor<ADBackend, 1> = scalar(r0, &device).require_grad();
    let npv = swap_ad.npv(r0_var.clone(), 0.0, &model_ad);
    let grads = npv.backward();
    let aad_delta = to_f64(r0_var.grad(&grads).unwrap());

    let bump_delta = central_diff(
        |r| to_f64(swap_f64.npv(scalar(r, &device), 0.0, &model_f64)),
        r0,
        BUMP_H,
    );

    assert!(
        (aad_delta - bump_delta).abs() < notional * TOLERANCE,
        "delta NPV IRS AAD={aad_delta} vs bump-and-reval={bump_delta}"
    );
}
