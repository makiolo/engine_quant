//! Ejemplo de la capa Rust (PLAN.md §7.18): calibra los dos modelos del motor --
//! `HullWhite1F` y `HullWhite2F`/G2++ -- a una `Curve` fabricada (no hay datos de
//! mercado reales en este árbol todavía, ver `Curve::synthetic_from_hull_white*`),
//! partiendo de una estimación inicial deliberadamente alejada de los parámetros "verdaderos",
//! y usa el `CalibrationResult` de cada uno para reconstruir el modelo ya calibrado -- cerrando
//! el círculo Mercado -> calibrar -> Modelo, igual que hacen los tests de las cinco capas
//! (PLAN.md §7.14/§7.16).
//!
//! `cargo run -p engine-core --example calibrate_models`

use engine_core::calibration::{calibrate_hull_white, calibrate_hull_white_2f};
use engine_core::curve::Curve;
use engine_core::models::hull_white::HullWhite1F;
use engine_core::models::hull_white_2f::HullWhite2F;

fn main() {
    let pillars = vec![0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0];

    // --- HullWhite1F: calibra (a, b); sigma/r0 son datos de entrada (PLAN.md §7.18). ---
    let (true_a, true_b, sigma, r0) = (0.15, 0.025, 0.008, 0.02);
    let curve_1f = Curve::synthetic_from_hull_white(true_a, true_b, sigma, r0, pillars.clone());

    let result_1f = calibrate_hull_white(&curve_1f, /* initial_a */ 0.3, /* initial_b */ 0.01, sigma, r0);
    println!(
        "HullWhite1F: a={:.6} b={:.6} rmse={:.3e} iterations={} converged={}",
        result_1f.a, result_1f.b, result_1f.rmse, result_1f.iterations, result_1f.converged
    );

    // El resultado alimenta directamente el modelo calibrado -- mismo tipo de valores que
    // recibiría HullWhite1F::new / ENGINE.CREATE_MODEL("HullWhite1F", ...).
    let device = Default::default();
    let scalar = |v: f64| burn::tensor::Tensor::<engine_core::backend::CpuBackend, 1>::from_data(
        burn::tensor::TensorData::from([v]),
        &device,
    );
    let calibrated_1f: HullWhite1F<engine_core::backend::CpuBackend> =
        HullWhite1F::new(scalar(result_1f.a), scalar(result_1f.b), scalar(result_1f.sigma));
    let price = calibrated_1f.zero_coupon_bond(scalar(result_1f.r0), 0.0, 10.0);
    println!("  P(0,10) bajo el modelo calibrado = {:.6}", price.into_scalar());

    // --- HullWhite2F/G2++: calibra (a, b), las dos velocidades de reversión; sigma/eta/rho/r0
    // son datos de entrada -- distinto subconjunto que HullWhite1F (PLAN.md §7.18: en G2++ "b"
    // es una velocidad de reversión, no un nivel de largo plazo). ---
    let (true_a_2f, true_b_2f, sigma_2f, eta_2f, rho_2f, r0_2f) = (0.15, 0.25, 0.008, 0.01, -0.6, 0.02);
    let curve_2f =
        Curve::synthetic_from_hull_white_2f(true_a_2f, true_b_2f, sigma_2f, eta_2f, rho_2f, r0_2f, pillars);

    let result_2f = calibrate_hull_white_2f(
        &curve_2f, /* initial_a */ 0.4, /* initial_b */ 0.05, sigma_2f, eta_2f, rho_2f, r0_2f,
    );
    println!(
        "HullWhite2F: a={:.6} b={:.6} rmse={:.3e} iterations={} converged={}",
        result_2f.a, result_2f.b, result_2f.rmse, result_2f.iterations, result_2f.converged
    );

    let calibrated_2f: HullWhite2F<engine_core::backend::CpuBackend> = HullWhite2F::new(
        scalar(result_2f.a),
        scalar(result_2f.b),
        scalar(result_2f.sigma),
        scalar(result_2f.eta),
        result_2f.rho,
        scalar(result_2f.r0),
    );
    let zero = scalar(0.0);
    let price_2f = calibrated_2f.zero_coupon_bond(zero.clone(), zero, 0.0, 10.0);
    println!("  P(0,10) bajo el modelo calibrado = {:.6}", price_2f.into_scalar());

    assert!(result_1f.converged && result_2f.converged, "ambas calibraciones deben converger");
}
