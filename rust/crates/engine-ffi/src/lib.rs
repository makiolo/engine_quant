//! Frontera cxx entre el core Rust (`engine-core`) y la capa de orquestación C++ (PLAN.md §7).
//! Este crate no contiene lógica de negocio, solo la traducción de la API pública de
//! `engine-core` a algo que `cxx` pueda exponer a C++.

#[cxx::bridge(namespace = "engine::ffi")]
mod ffi {
    /// Resultado plano de un perfil de exposición (PLAN.md §5.5: "los tipos complejos ...
    /// se pasan mediante structs planos"), tres vectores paralelos indexados por fecha de
    /// monitorización.
    struct ExposureProfileResult {
        times: Vec<f64>,
        ee: Vec<f64>,
        pfe_95: Vec<f64>,
    }

    extern "Rust" {
        fn ping() -> f64;

        // Cadena de humo ampliada (PLAN.md §7.1): ejercita Hull-White + IRS + exposición/CVA
        // + AAD, ya sobre Burn (PLAN.md §5.1, §5.3). Pura traducción de tipos: la lógica
        // vive en `engine_core::smoke`, que no conoce `cxx` (PLAN.md §7: frontera aislada).
        fn hull_white_zero_coupon_bond(a: f64, b: f64, sigma: f64, r0: f64, t: f64, maturity: f64) -> f64;
        fn hull_white_zero_coupon_bond_delta_r0(
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            t: f64,
            maturity: f64,
        ) -> f64;
        fn irs_unilateral_cva_5y(
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            notional: f64,
            hazard_rate: f64,
            recovery_rate: f64,
            n_paths: u64,
            seed: u64,
        ) -> f64;

        // API generalizada de Fase 2 (PLAN.md §5.4, §6): a diferencia de
        // `irs_unilateral_cva_5y` (cadena de humo de Fase 1, IRS 5y anual fijo), estas dos
        // funciones aceptan un IRS arbitrario (fechas de pago/accruals propios) y separan el
        // cálculo del perfil de exposición del cálculo del CVA a partir de ese perfil, para
        // que el registry de medidas de la capa C++ (`cpp/engine/include/engine/measure.hpp`)
        // pueda componerlas.
        fn irs_hull_white_exposure_profile(
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            notional: f64,
            fixed_rate: f64,
            use_par_rate: bool,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
            monitoring_times: Vec<f64>,
            n_paths: u64,
            seed: u64,
        ) -> ExposureProfileResult;

        fn unilateral_cva_from_exposure(
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            times: Vec<f64>,
            ee: Vec<f64>,
            hazard_rate: f64,
            recovery_rate: f64,
        ) -> f64;
    }
}

fn ping() -> f64 {
    engine_core::ping()
}

fn hull_white_zero_coupon_bond(a: f64, b: f64, sigma: f64, r0: f64, t: f64, maturity: f64) -> f64 {
    engine_core::smoke::hull_white_zero_coupon_bond(a, b, sigma, r0, t, maturity)
}

fn hull_white_zero_coupon_bond_delta_r0(a: f64, b: f64, sigma: f64, r0: f64, t: f64, maturity: f64) -> f64 {
    engine_core::smoke::hull_white_zero_coupon_bond_delta_r0(a, b, sigma, r0, t, maturity)
}

fn irs_unilateral_cva_5y(
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
    engine_core::smoke::irs_unilateral_cva_5y(
        a,
        b,
        sigma,
        r0,
        notional,
        hazard_rate,
        recovery_rate,
        n_paths,
        seed,
    )
}

fn irs_hull_white_exposure_profile(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: Vec<f64>,
    n_paths: u64,
    seed: u64,
) -> ffi::ExposureProfileResult {
    let profile = engine_core::api::irs_hull_white_exposure_profile(
        a,
        b,
        sigma,
        r0,
        notional,
        fixed_rate,
        use_par_rate,
        start,
        payment_times,
        accruals,
        &monitoring_times,
        n_paths as usize,
        seed,
    );
    ffi::ExposureProfileResult {
        times: profile.times,
        ee: profile.ee,
        pfe_95: profile.pfe_95,
    }
}

fn unilateral_cva_from_exposure(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    engine_core::api::unilateral_cva_from_exposure(a, b, sigma, r0, times, ee, hazard_rate, recovery_rate)
}
