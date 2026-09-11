//! Ejemplo de la capa Rust (PLAN.md §7.19): calibra el lote homogéneo, la parte que de
//! verdad vive en Rust de `price_batch`/`price_many`/`price_grid` (la orquestación -- agrupar,
//! trasladar nombres de medida, recomponer resultados -- vive en la capa C++, `engine::price_
//! batch`/`price_many`/`price_grid`, ver `cpp/engine/src/price.cpp`). Aquí se ejercitan
//! directamente los primitivos `f64` puros de `engine_core::api` que esa capa C++ consume:
//! un lote de 3 swaps del mismo calendario, valorados con las 5 medidas de una sola llamada
//! cada una, comparado contra un bucle de llamadas escalares para demostrar que el resultado
//! es idéntico.
//!
//! `cargo run -p engine-core --example price_batch`

use engine_core::api::{
    irs_hull_white_exposure_profile, irs_hull_white_exposure_profile_batch, irs_hull_white_npv,
    irs_hull_white_npv_batch, irs_hull_white_npv_delta_r0, irs_hull_white_npv_delta_r0_batch,
    unilateral_cva_from_exposure, unilateral_cva_from_exposure_batch,
};

fn main() {
    let (a, b, sigma, r0) = (0.1, 0.03, 0.01, 0.02);
    let start = 0.0;
    let payment_times = vec![1.0, 2.0, 3.0, 4.0, 5.0];
    let accruals = vec![1.0; 5];
    let notionals = vec![1_000_000.0, 2_500_000.0, 500_000.0];
    let fixed_rates = vec![0.02, 0.015, 0.025];
    let monitoring_times = vec![0.0, 1.0, 2.0, 3.0, 4.0];
    let (n_steps, n_paths, seed) = (208, 5_000, 7);
    let (hazard_rate, recovery_rate) = (0.02, 0.4);

    // --- PV/DV01: "gratis" por broadcasting (PLAN.md §7.17/§7.19) -- irs_hull_white_npv_batch
    // ya existía, aquí se ejercita junto al resto para tener el mismo ejemplo completo. ---
    let pv_batch = irs_hull_white_npv_batch(
        a, b, sigma, r0, notionals.clone(), fixed_rates.clone(), start, payment_times.clone(), accruals.clone(),
    );
    let dv01_batch = irs_hull_white_npv_delta_r0_batch(
        a, b, sigma, r0, notionals.clone(), fixed_rates.clone(), start, payment_times.clone(), accruals.clone(),
    );

    // --- ExpectedExposure/PFE95/UnilateralCVA: simula el tipo corto UNA sola vez para todo el
    // lote (todos los trades comparten escenario), revalora los 3 swaps a la vez en cada
    // fecha de monitorización. ---
    let profiles = irs_hull_white_exposure_profile_batch(
        "cpu", a, b, sigma, r0, notionals.clone(), fixed_rates.clone(), start, payment_times.clone(),
        accruals.clone(), &monitoring_times, n_steps, n_paths, seed,
    );
    let cva_batch = unilateral_cva_from_exposure_batch(
        "cpu", a, b, sigma, r0, profiles.clone(), hazard_rate, recovery_rate,
    );

    println!("{:>10} {:>12} {:>10} {:>10}", "trade", "PV", "DV01", "CVA");
    for i in 0..notionals.len() {
        println!("{:>10} {:>12.2} {:>10.4} {:>10.2}", i, pv_batch[i], dv01_batch[i], cva_batch[i]);
    }

    // --- Verificación: el lote debe coincidir, swap a swap, con un bucle de llamadas
    // escalares (mismo patrón que los tests de `irs.rs`/`exposure.rs`/`api.rs`). ---
    for i in 0..notionals.len() {
        let scalar_pv = irs_hull_white_npv(
            a, b, sigma, r0, notionals[i], fixed_rates[i], false, start, payment_times.clone(), accruals.clone(),
        );
        let scalar_dv01 = irs_hull_white_npv_delta_r0(
            a, b, sigma, r0, notionals[i], fixed_rates[i], false, start, payment_times.clone(), accruals.clone(),
        );
        let scalar_profile = irs_hull_white_exposure_profile(
            "cpu", a, b, sigma, r0, notionals[i], fixed_rates[i], false, start, payment_times.clone(),
            accruals.clone(), &monitoring_times, n_steps, n_paths, seed,
        );
        let scalar_cva = unilateral_cva_from_exposure(
            "cpu", a, b, sigma, r0, scalar_profile.times.clone(), scalar_profile.ee.clone(), hazard_rate, recovery_rate,
        );

        assert!((pv_batch[i] - scalar_pv).abs() < 1e-6);
        assert!((dv01_batch[i] - scalar_dv01).abs() < 1e-9);
        assert!((cva_batch[i] - scalar_cva).abs() < 1e-6);
        for k in 0..monitoring_times.len() {
            assert!((profiles[i].ee[k] - scalar_profile.ee[k]).abs() < 1e-6);
            assert!((profiles[i].pfe_95[k] - scalar_profile.pfe_95[k]).abs() < 1e-6);
        }
    }
    println!("OK: el lote coincide, swap a swap, con un bucle de llamadas escalares.");
}
