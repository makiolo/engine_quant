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

    /// Resultado plano de calibrar `HullWhite1F` a un mercado (PLAN.md §7.14:
    /// `crate::calibration::HullWhiteCalibrationResult`, ver ese módulo para el porqué solo
    /// `a`/`b` se calibran).
    struct HullWhiteCalibrationResult {
        a: f64,
        b: f64,
        sigma: f64,
        r0: f64,
        rmse: f64,
        iterations: u32,
        converged: bool,
    }

    /// Equivalente de dos factores de `HullWhiteCalibrationResult` (PLAN.md §7.18:
    /// `crate::calibration::HullWhite2FCalibrationResult`).
    struct HullWhite2FCalibrationResult {
        a: f64,
        b: f64,
        sigma: f64,
        eta: f64,
        rho: f64,
        r0: f64,
        rmse: f64,
        iterations: u32,
        converged: bool,
    }

    /// Precio bajo Q de un `PayoffProgram` (PLAN_PRODUCTS.md §12 Fase 5:
    /// `engine_core::mc::McEstimate`) -- media, error estandar e intervalo de confianza del
    /// estimador Monte Carlo, ver `engine_core::payoff::price_payoff_gbm_q`.
    struct PayoffQPriceResult {
        mean: f64,
        std_error: f64,
        ci_low: f64,
        ci_high: f64,
        n_paths: u64,
    }

    /// Probabilidad de hit bajo Q de un evento (`Trigger`) de un `PayoffProgram`
    /// (PLAN_PRODUCTS.md §12 Fase 6: `engine_core::mc::McEstimate` sobre un indicador 0/1 de "el
    /// evento ocurrio en esta ruta", ver `engine_core::payoff::hit_probability_gbm_q`). Misma
    /// forma que `PayoffQPriceResult` pero `mean` es una probabilidad en `[0,1]`, no un valor
    /// monetario -- struct separado para que el nombre no induzca a leerlo como precio.
    struct PayoffQHitProbabilityResult {
        probability: f64,
        std_error: f64,
        ci_low: f64,
        ci_high: f64,
        n_paths: u64,
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
        // pueda componerlas. `backend` ("cpu"/"gpu") es un parámetro explícito de cada
        // llamada desde PLAN.md §7.15 — ya no hay estado global de backend (§7.12, retirado).
        fn irs_hull_white_exposure_profile(
            backend: String,
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
            n_steps: u64,
            n_paths: u64,
            seed: u64,
        ) -> ExposureProfileResult;

        fn unilateral_cva_from_exposure(
            backend: String,
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            times: Vec<f64>,
            ee: Vec<f64>,
            hazard_rate: f64,
            recovery_rate: f64,
        ) -> f64;

        fn is_gpu_backend_available() -> bool;

        // NPV determinista del IRS a t=0 y su sensibilidad a r0 (PLAN.md §7.15: medidas "PV"/
        // "DV01" de ENGINE.PRICE) — siempre en CpuBackend, ver `crate::api` para el porqué.
        fn irs_hull_white_npv(
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
        ) -> f64;

        fn irs_hull_white_npv_delta_r0(
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
        ) -> f64;

        // Lote homogéneo (PLAN.md §7.17/§7.19): las cinco medidas de ENGINE.PRICE vectorizadas
        // sobre N trades del mismo tipo/calendario, sin bucle escalar en la frontera C++ --
        // notionals/fixed_rates son columnas, un valor por trade. `irs_hull_white_npv_batch`
        // ya existía en `engine_core::api` pero sin bridgear a C++ (§7.17 la dejó como
        // primitivo interno de Rust); las otras cuatro son nuevas en esta fase.
        fn irs_hull_white_npv_batch(
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            notionals: Vec<f64>,
            fixed_rates: Vec<f64>,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
        ) -> Vec<f64>;

        // No es una sola pasada backward() para todo el lote -- ver
        // `engine_core::api::irs_hull_white_npv_delta_r0_batch` para el porqué (reverse-mode
        // AD con un r0 compartido solo da la suma de sensibilidades en una pasada, no cada una
        // por separado). El lote evita N *round-trips* de FFI/C++/Python/Excel, no las N
        // pasadas backward en sí.
        fn irs_hull_white_npv_delta_r0_batch(
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            notionals: Vec<f64>,
            fixed_rates: Vec<f64>,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
        ) -> Vec<f64>;

        fn irs_hull_white_exposure_profile_batch(
            backend: String,
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            notionals: Vec<f64>,
            fixed_rates: Vec<f64>,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
            monitoring_times: Vec<f64>,
            n_steps: u64,
            n_paths: u64,
            seed: u64,
        ) -> Vec<ExposureProfileResult>;

        // `profiles` es el mismo `Vec<ExposureProfileResult>` que ya devuelve
        // `irs_hull_white_exposure_profile_batch` -- se pasa de vuelta tal cual, sin volver a
        // calcular el perfil (mismo espíritu que `unilateral_cva_from_exposure` reutilizando
        // `times`/`ee` ya calculados en la versión escalar).
        fn unilateral_cva_from_exposure_batch(
            backend: String,
            a: f64,
            b: f64,
            sigma: f64,
            r0: f64,
            profiles: Vec<ExposureProfileResult>,
            hazard_rate: f64,
            recovery_rate: f64,
        ) -> Vec<f64>;

        // Segundo modelo del motor, Hull-White 2 factores (PLAN.md §7.16, G2++): mismas seis
        // funciones que su equivalente de 1 factor arriba, mismo shape de resultado
        // (`ExposureProfileResult`) -- la capa C++ (`engine/measure.hpp`) las consume con el
        // mismo código de medida, solo cambiando qué wrapper llama según el modelo recibido.
        fn irs_hull_white_2f_exposure_profile(
            backend: String,
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            notional: f64,
            fixed_rate: f64,
            use_par_rate: bool,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
            monitoring_times: Vec<f64>,
            n_steps: u64,
            n_paths: u64,
            seed: u64,
        ) -> ExposureProfileResult;

        fn unilateral_cva_from_exposure_2f(
            backend: String,
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            times: Vec<f64>,
            ee: Vec<f64>,
            hazard_rate: f64,
            recovery_rate: f64,
        ) -> f64;

        fn irs_hull_white_2f_npv(
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            notional: f64,
            fixed_rate: f64,
            use_par_rate: bool,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
        ) -> f64;

        fn irs_hull_white_2f_npv_delta_r0(
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            notional: f64,
            fixed_rate: f64,
            use_par_rate: bool,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
        ) -> f64;

        // Equivalentes de lote de las cuatro funciones 2F de arriba -- ver las versiones de 1
        // factor para el porqué de cada una (PLAN.md §7.19).
        fn irs_hull_white_2f_npv_batch(
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            notionals: Vec<f64>,
            fixed_rates: Vec<f64>,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
        ) -> Vec<f64>;

        fn irs_hull_white_2f_npv_delta_r0_batch(
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            notionals: Vec<f64>,
            fixed_rates: Vec<f64>,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
        ) -> Vec<f64>;

        fn irs_hull_white_2f_exposure_profile_batch(
            backend: String,
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            notionals: Vec<f64>,
            fixed_rates: Vec<f64>,
            start: f64,
            payment_times: Vec<f64>,
            accruals: Vec<f64>,
            monitoring_times: Vec<f64>,
            n_steps: u64,
            n_paths: u64,
            seed: u64,
        ) -> Vec<ExposureProfileResult>;

        fn unilateral_cva_from_exposure_2f_batch(
            backend: String,
            a: f64,
            b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
            profiles: Vec<ExposureProfileResult>,
            hazard_rate: f64,
            recovery_rate: f64,
        ) -> Vec<f64>;

        // Calibración de mercado (PLAN.md §7.14, §7.20): pillars/zero_rates es la Curve en
        // su forma más plana (dos vectores paralelos, PLAN.md §5.5), ver
        // `engine_core::curve`/`engine_core::calibration`.
        fn calibrate_hull_white(
            pillars: Vec<f64>,
            zero_rates: Vec<f64>,
            initial_a: f64,
            initial_b: f64,
            sigma: f64,
            r0: f64,
        ) -> HullWhiteCalibrationResult;

        // Equivalente de dos factores (PLAN.md §7.18): calibra a/b de HullWhite2F, ver
        // engine_core::calibration para el porqué del resto de parámetros fijos.
        fn calibrate_hull_white_2f(
            pillars: Vec<f64>,
            zero_rates: Vec<f64>,
            initial_a: f64,
            initial_b: f64,
            sigma: f64,
            eta: f64,
            rho: f64,
            r0: f64,
        ) -> HullWhite2FCalibrationResult;

        // Precio del bono cero-cupón de HullWhite2F con los dos factores latentes en su valor
        // inicial (PLAN.md §7.18) -- equivalente de dos factores de
        // hull_white_zero_coupon_bond, usado por engine::Curve::synthetic_from_hull_white_2f
        // en C++ (PLAN.md §7.20) para fabricar una curva sin datos reales.
        fn hull_white_2f_zero_coupon_bond(a: f64, b: f64, sigma: f64, eta: f64, rho: f64, r0: f64, maturity: f64) -> f64;

        // PLAN_PRODUCTS.md §12 Fase 5: precio bajo Q (GBM) de un PayoffProgram serializado como
        // JSON canonico engine.payoff/v1 (el mismo que produce CanonicalVisitor::to_json en
        // C++/engine_typed.payoff en Python) -- ver engine_core::payoff, que compila/evalua el
        // IR enteramente en Rust; solo el resultado final cruza esta frontera. A diferencia de
        // toda funcion de arriba, esta SI puede fallar (preflight de "observable no generado" u
        // otro error de compilacion del JSON) -- `Result<T>` hace que un `Err(String)` del lado
        // Rust cruce como una excepcion de C++ en el punto de la llamada (ver cpp/engine/src/
        // payoff/measures.cpp, que la captura y la reexpone como ValidationError/EvaluationError).
        fn price_payoff_gbm_q(
            spec_json: String,
            observable: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffQPriceResult>;

        // PLAN_PRODUCTS.md §12 Fase 6: probabilidad bajo Q de que `event` (un Trigger del
        // contrato) dispare, sobre las mismas rutas GBM que usaria `price_payoff_gbm_q` para el
        // mismo `spec_json`/modelo. Preflight identico (observable no generado) mas "el evento
        // '{event}' no existe en este contrato" si `event` no coincide con ningun `EventId`
        // declarado -- ver engine_core::payoff::hit_probability_gbm_q.
        fn hit_probability_gbm_q(
            spec_json: String,
            event: String,
            observable: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffQHitProbabilityResult>;
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

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_exposure_profile(
    backend: String,
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
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> ffi::ExposureProfileResult {
    let profile = engine_core::api::irs_hull_white_exposure_profile(
        &backend,
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
        n_steps as usize,
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
    backend: String,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    engine_core::api::unilateral_cva_from_exposure(&backend, a, b, sigma, r0, times, ee, hazard_rate, recovery_rate)
}

fn is_gpu_backend_available() -> bool {
    engine_core::api::is_gpu_backend_available()
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_npv(
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
) -> f64 {
    engine_core::api::irs_hull_white_npv(a, b, sigma, r0, notional, fixed_rate, use_par_rate, start, payment_times, accruals)
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_npv_delta_r0(
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
) -> f64 {
    engine_core::api::irs_hull_white_npv_delta_r0(
        a, b, sigma, r0, notional, fixed_rate, use_par_rate, start, payment_times, accruals,
    )
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_npv_batch(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    engine_core::api::irs_hull_white_npv_batch(a, b, sigma, r0, notionals, fixed_rates, start, payment_times, accruals)
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_npv_delta_r0_batch(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    engine_core::api::irs_hull_white_npv_delta_r0_batch(a, b, sigma, r0, notionals, fixed_rates, start, payment_times, accruals)
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_exposure_profile_batch(
    backend: String,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: Vec<f64>,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> Vec<ffi::ExposureProfileResult> {
    let profiles = engine_core::api::irs_hull_white_exposure_profile_batch(
        &backend, a, b, sigma, r0, notionals, fixed_rates, start, payment_times, accruals,
        &monitoring_times, n_steps as usize, n_paths as usize, seed,
    );
    profiles
        .into_iter()
        .map(|p| ffi::ExposureProfileResult { times: p.times, ee: p.ee, pfe_95: p.pfe_95 })
        .collect()
}

#[allow(clippy::too_many_arguments)]
fn unilateral_cva_from_exposure_batch(
    backend: String,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    profiles: Vec<ffi::ExposureProfileResult>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> Vec<f64> {
    let profiles = profiles
        .into_iter()
        .map(|p| engine_core::exposure::ExposureProfile { times: p.times, ee: p.ee, pfe_95: p.pfe_95 })
        .collect();
    engine_core::api::unilateral_cva_from_exposure_batch(&backend, a, b, sigma, r0, profiles, hazard_rate, recovery_rate)
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_2f_exposure_profile(
    backend: String,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: Vec<f64>,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> ffi::ExposureProfileResult {
    let profile = engine_core::api::irs_hull_white_2f_exposure_profile(
        &backend,
        a,
        b,
        sigma,
        eta,
        rho,
        r0,
        notional,
        fixed_rate,
        use_par_rate,
        start,
        payment_times,
        accruals,
        &monitoring_times,
        n_steps as usize,
        n_paths as usize,
        seed,
    );
    ffi::ExposureProfileResult {
        times: profile.times,
        ee: profile.ee,
        pfe_95: profile.pfe_95,
    }
}

#[allow(clippy::too_many_arguments)]
fn unilateral_cva_from_exposure_2f(
    backend: String,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    times: Vec<f64>,
    ee: Vec<f64>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> f64 {
    engine_core::api::unilateral_cva_from_exposure_2f(&backend, a, b, sigma, eta, rho, r0, times, ee, hazard_rate, recovery_rate)
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_2f_npv(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> f64 {
    engine_core::api::irs_hull_white_2f_npv(a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start, payment_times, accruals)
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_2f_npv_delta_r0(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> f64 {
    engine_core::api::irs_hull_white_2f_npv_delta_r0(
        a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start, payment_times, accruals,
    )
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_2f_npv_batch(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    engine_core::api::irs_hull_white_2f_npv_batch(a, b, sigma, eta, rho, r0, notionals, fixed_rates, start, payment_times, accruals)
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_2f_npv_delta_r0_batch(
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
) -> Vec<f64> {
    engine_core::api::irs_hull_white_2f_npv_delta_r0_batch(
        a, b, sigma, eta, rho, r0, notionals, fixed_rates, start, payment_times, accruals,
    )
}

#[allow(clippy::too_many_arguments)]
fn irs_hull_white_2f_exposure_profile_batch(
    backend: String,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    notionals: Vec<f64>,
    fixed_rates: Vec<f64>,
    start: f64,
    payment_times: Vec<f64>,
    accruals: Vec<f64>,
    monitoring_times: Vec<f64>,
    n_steps: u64,
    n_paths: u64,
    seed: u64,
) -> Vec<ffi::ExposureProfileResult> {
    let profiles = engine_core::api::irs_hull_white_2f_exposure_profile_batch(
        &backend, a, b, sigma, eta, rho, r0, notionals, fixed_rates, start, payment_times, accruals,
        &monitoring_times, n_steps as usize, n_paths as usize, seed,
    );
    profiles
        .into_iter()
        .map(|p| ffi::ExposureProfileResult { times: p.times, ee: p.ee, pfe_95: p.pfe_95 })
        .collect()
}

#[allow(clippy::too_many_arguments)]
fn unilateral_cva_from_exposure_2f_batch(
    backend: String,
    a: f64,
    b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
    profiles: Vec<ffi::ExposureProfileResult>,
    hazard_rate: f64,
    recovery_rate: f64,
) -> Vec<f64> {
    let profiles = profiles
        .into_iter()
        .map(|p| engine_core::exposure::ExposureProfile { times: p.times, ee: p.ee, pfe_95: p.pfe_95 })
        .collect();
    engine_core::api::unilateral_cva_from_exposure_2f_batch(&backend, a, b, sigma, eta, rho, r0, profiles, hazard_rate, recovery_rate)
}

fn calibrate_hull_white(
    pillars: Vec<f64>,
    zero_rates: Vec<f64>,
    initial_a: f64,
    initial_b: f64,
    sigma: f64,
    r0: f64,
) -> ffi::HullWhiteCalibrationResult {
    let result = engine_core::api::calibrate_hull_white(pillars, zero_rates, initial_a, initial_b, sigma, r0);
    ffi::HullWhiteCalibrationResult {
        a: result.a,
        b: result.b,
        sigma: result.sigma,
        r0: result.r0,
        rmse: result.rmse,
        iterations: result.iterations,
        converged: result.converged,
    }
}

#[allow(clippy::too_many_arguments)]
fn calibrate_hull_white_2f(
    pillars: Vec<f64>,
    zero_rates: Vec<f64>,
    initial_a: f64,
    initial_b: f64,
    sigma: f64,
    eta: f64,
    rho: f64,
    r0: f64,
) -> ffi::HullWhite2FCalibrationResult {
    let result = engine_core::api::calibrate_hull_white_2f(pillars, zero_rates, initial_a, initial_b, sigma, eta, rho, r0);
    ffi::HullWhite2FCalibrationResult {
        a: result.a,
        b: result.b,
        sigma: result.sigma,
        eta: result.eta,
        rho: result.rho,
        r0: result.r0,
        rmse: result.rmse,
        iterations: result.iterations,
        converged: result.converged,
    }
}

fn hull_white_2f_zero_coupon_bond(a: f64, b: f64, sigma: f64, eta: f64, rho: f64, r0: f64, maturity: f64) -> f64 {
    engine_core::smoke::hull_white_2f_zero_coupon_bond(a, b, sigma, eta, rho, r0, maturity)
}

#[allow(clippy::too_many_arguments)]
fn price_payoff_gbm_q(
    spec_json: String,
    observable: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffQPriceResult, String> {
    let estimate =
        engine_core::payoff::price_payoff_gbm_q(&spec_json, &observable, s0, r, q, sigma, n_paths, seed)?;
    Ok(ffi::PayoffQPriceResult {
        mean: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn hit_probability_gbm_q(
    spec_json: String,
    event: String,
    observable: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffQHitProbabilityResult, String> {
    let estimate =
        engine_core::payoff::hit_probability_gbm_q(&spec_json, &event, &observable, s0, r, q, sigma, n_paths, seed)?;
    Ok(ffi::PayoffQHitProbabilityResult {
        probability: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}
