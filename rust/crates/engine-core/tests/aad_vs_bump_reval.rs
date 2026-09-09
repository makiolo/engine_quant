//! PLAN.md §5.6 capa 3: toda sensibilidad calculada vía AAD se contrasta contra su
//! equivalente por diferencias finitas (bump-and-reval), dentro de una tolerancia acorde
//! al step de bump usado. Vive como test de integración (no dentro de un módulo) porque
//! ejercita `dual`, `models::hull_white` e `products::irs` juntos — es la validación de
//! que el mecanismo de AAD elegido en Fase 1 (PLAN.md §5.3) es correcto, no solo que cada
//! pieza compila.

use engine_core::dual::Dual;
use engine_core::models::hull_white::HullWhite1F;
use engine_core::products::irs::IrSwap;

/// Diferencia central de segundo orden: error de truncamiento `O(h^2)`.
fn central_diff<F: Fn(f64) -> f64>(f: F, x: f64, h: f64) -> f64 {
    (f(x + h) - f(x - h)) / (2.0 * h)
}

const BUMP_H: f64 = 1e-6;
// Con h=1e-6 el error de truncamiento de la diferencia central es O(h^2)=1e-12 y el de
// redondeo de punto flotante ~1e-16/h=1e-10; 1e-6 de tolerancia absoluta deja margen
// generoso para la curvatura real de estas funciones sin ocultar un error de signo o de
// regla de la cadena en la implementación de `Dual`.
const TOLERANCE: f64 = 1e-6;

#[test]
fn aad_delta_of_zero_coupon_bond_wrt_r0_matches_bump_and_reval() {
    let (a, b, sigma, r0, t, maturity) = (0.1, 0.03, 0.01, 0.02, 0.0, 5.0);

    let model_dual = HullWhite1F::new(Dual::constant(a), Dual::constant(b), Dual::constant(sigma));
    let aad_delta = model_dual
        .zero_coupon_bond(Dual::variable(r0), t, maturity)
        .derivative();

    let model_f64 = HullWhite1F::new(a, b, sigma);
    let bump_delta = central_diff(
        |r| model_f64.zero_coupon_bond(r, t, maturity),
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

    let model_dual = HullWhite1F::new(
        Dual::constant(a),
        Dual::constant(b),
        Dual::variable(sigma),
    );
    let aad_vega = model_dual.zero_coupon_bond(Dual::constant(r0), t, maturity).derivative();

    let bump_vega = central_diff(
        |s| HullWhite1F::new(a, b, s).zero_coupon_bond(r0, t, maturity),
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

    let model_dual = HullWhite1F::new(Dual::variable(a), Dual::constant(b), Dual::constant(sigma));
    let aad_sens = model_dual.zero_coupon_bond(Dual::constant(r0), t, maturity).derivative();

    let bump_sens = central_diff(
        |a_bumped| HullWhite1F::new(a_bumped, b, sigma).zero_coupon_bond(r0, t, maturity),
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

    let model_f64 = HullWhite1F::new(a, b, sigma);
    let fixed_rate = IrSwap::par_rate(notional, r0, 0.0, &payment_times, &accruals, &model_f64);

    let swap_f64 = IrSwap {
        notional,
        fixed_rate,
        start: 0.0,
        payment_times: payment_times.clone(),
        accruals: accruals.clone(),
    };

    let model_dual = HullWhite1F::new(Dual::constant(a), Dual::constant(b), Dual::constant(sigma));
    let swap_dual = IrSwap {
        notional: Dual::constant(notional),
        fixed_rate: Dual::constant(fixed_rate),
        start: 0.0,
        payment_times: payment_times.clone(),
        accruals: accruals.clone(),
    };

    let aad_delta = swap_dual.npv(Dual::variable(r0), 0.0, &model_dual).derivative();
    let bump_delta = central_diff(|r| swap_f64.npv(r, 0.0, &model_f64), r0, BUMP_H);

    assert!(
        (aad_delta - bump_delta).abs() < notional * TOLERANCE,
        "delta NPV IRS AAD={aad_delta} vs bump-and-reval={bump_delta}"
    );
}
