//! Frontera cxx entre el core Rust (`engine-core`) y la capa de orquestación C++ (PLAN.md §7).
//! Este crate no contiene lógica de negocio, solo la traducción de la API pública de
//! `engine-core` a algo que `cxx` pueda exponer a C++.

#[cxx::bridge(namespace = "engine::ffi")]
mod ffi {
    /// Resultado plano de un perfil de exposición (PLAN.md §5.5: "los tipos complejos ...
    /// se pasan mediante structs planos"), tres vectores paralelos indexados por fecha de
    /// monitorización. Reutilizado tal cual por `payoff_exposure_profile_gbm_q`
    /// (PLAN_PRODUCTS.md §12 Fase 6, `engine_core::exposure::ExposureProfile`) -- misma forma,
    /// sin inventar un tipo de resultado nuevo (§5.5).
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

    /// "Forecast" bajo P de un `PayoffProgram` (PLAN_PRODUCTS.md §12 Fase 7:
    /// `engine_core::mc::McEstimate` sobre la suma de cashflows SIN DESCONTAR -- ver el
    /// doc-comment de `engine_core::payoff::api_p`, "P se reserva para forecast ... no existe un
    /// numerario libre de riesgo canonico"). Misma forma que `PayoffQPriceResult`, struct
    /// separado para que el nombre no sugiera un precio Q ni un valor descontado.
    struct PayoffPForecastResult {
        mean: f64,
        std_error: f64,
        ci_low: f64,
        ci_high: f64,
        n_paths: u64,
    }

    /// Equivalente bajo P de `PayoffQHitProbabilityResult` (PLAN_PRODUCTS.md §12 Fase 7:
    /// "HitProbabilityP"), ver `engine_core::payoff::hit_probability_gbm_p`.
    struct PayoffPHitProbabilityResult {
        probability: f64,
        std_error: f64,
        ci_low: f64,
        ci_high: f64,
        n_paths: u64,
    }

    /// Distribucion de P&L de una estrategia bajo P (PLAN_PRODUCTS.md §12 Fase 7: "distribucion
    /// de P&L y expected shortfall de estrategia"), ver `engine_core::payoff::PnlDistribution`
    /// para la convencion de signo de `var`/`es` (perdidas positivas, `es >= var` siempre).
    struct PnlDistributionResult {
        mean: f64,
        std_error: f64,
        var: f64,
        es: f64,
        n_paths: u64,
    }

    /// Diagnostico de UNA fecha de decision de un `Exercise` (PLAN_PRODUCTS.md §10, Fase 9:
    /// "diagnostico de regresion y politica de ejercicio exportable"), ver
    /// `engine_core::payoff::ExerciseDateDiagnostic`. `has_regression=false` cuando en `date` no
    /// hubo suficientes rutas in-the-money para ajustar la regresion (`coeff_a/b/c` son 0.0 en ese
    /// caso, no un ajuste real -- `has_regression` es la unica forma de distinguirlo de un ajuste
    /// legitimo de coeficientes nulos).
    struct ExerciseDateDiagnosticResult {
        date: f64,
        n_in_the_money: u64,
        has_regression: bool,
        coeff_a: f64,
        coeff_b: f64,
        coeff_c: f64,
        exercised_fraction: f64,
    }

    /// Precio bajo Q de un `PayoffProgram` con un derecho de ejercicio (PLAN_PRODUCTS.md §10,
    /// Fase 9), ver `engine_core::payoff::ExercisePolicyResult`: misma forma de precio que
    /// `PayoffQPriceResult` (`mean`/`std_error`/`ci_low`/`ci_high`/`n_paths`) mas `dates`, el
    /// diagnostico de la politica de ejercicio resuelta, en el mismo orden ascendente que las
    /// `dates` declaradas en el contrato.
    struct ExercisePolicyResult {
        mean: f64,
        std_error: f64,
        ci_low: f64,
        ci_high: f64,
        n_paths: u64,
        dates: Vec<ExerciseDateDiagnosticResult>,
    }

    /// Sensibilidad ("Greek") pathwise bajo Q de un `PayoffProgram` respecto de uno de los cuatro
    /// parametros de `Gbm` (PLAN_PRODUCTS.md §12 Fase 11, item pendiente "cablear
    /// payoff::api::payoff_sensitivity_gbm_q ... al bridge cxx"), ver
    /// `engine_core::payoff::payoff_sensitivity_gbm_q`. Misma forma que `PayoffQPriceResult` pero
    /// `value` es una derivada (puede ser negativa), no un precio -- struct separado para que el
    /// nombre no sugiera un valor monetario absoluto.
    struct PayoffSensitivityResult {
        value: f64,
        std_error: f64,
        ci_low: f64,
        ci_high: f64,
        n_paths: u64,
    }

    /// Hessiano local (Gamma/Volga/Vanna) de un `PayoffProgram` bajo GBM, via likelihood ratio, en
    /// UNA SOLA tanda de rutas simuladas (PLAN_BACKWARD.md §9 Fase 1), ver
    /// `engine_core::payoff::LocalHessianEstimate`. Reutiliza `PayoffSensitivityResult` para cada
    /// componente (misma forma que `payoff_sensitivity2_gbm_q`/`payoff_sensitivity_cross_gbm_q`,
    /// sin inventar un formato nuevo).
    struct PayoffLocalHessianResult {
        gamma: PayoffSensitivityResult,
        volga: PayoffSensitivityResult,
        vanna: PayoffSensitivityResult,
    }

    /// Resultado de sintetizar una cobertura bajo GBM/Q (PLAN_PRODUCTS.md §11/§12 Fase 11, item
    /// pendiente "cablear payoff::hedge::synthesize_hedge_gbm_q ... al bridge cxx"), ver
    /// `engine_core::payoff::HedgeResult`. Los campos opcionales de `HedgeResult`
    /// (`cost`/`gross_notional`/`residual_greeks`, todos `Option` del lado Rust) cruzan como un
    /// par `has_*`/valor -- cxx no tiene `Option<f64>` nativo en una struct compartida, y un
    /// sentinel NaN se prestaria a propagarse en silencio si alguien olvida comprobarlo; un `bool`
    /// explicito no.
    struct HedgeSynthesisResult {
        weights: Vec<f64>,
        residuals: Vec<f64>,
        residual_mean: f64,
        residual_std: f64,
        residual_max_abs: f64,
        has_cost: bool,
        cost: f64,
        has_gross_notional: bool,
        gross_notional: f64,
        has_residual_greeks: bool,
        residual_greeks_delta: f64,
        residual_greeks_rho: f64,
        residual_greeks_dividend_yield: f64,
        residual_greeks_vega: f64,
    }

    /// Las cuatro derivadas de primer orden del NPV determinista de Hull-White 1F en una unica
    /// pasada AAD reverse-mode (PLAN_GREEKS.md §5.2/§11 Fase 7), ver
    /// `engine_core::api::HullWhite1FGreeks`/`irs_hull_white_npv_all_greeks`.
    struct HullWhite1FGreeksResult {
        d_a: f64,
        d_b: f64,
        d_sigma: f64,
        d_r0: f64,
    }

    /// Valor + Hessiano 4x4 completo (10 pares) del NPV determinista de Hull-White 1F
    /// (PLAN_BACKWARD.md §9 Fase 2), ver `engine_core::models::hull_white_dual::HullWhite1FHessian`/
    /// `hull_white_1f_hessian`. Calculado con `Dual2`/`HyperDual` NUEVOS (forward-over-forward
    /// cerrado, no AAD reverse-mode de Burn -- Burn no anida `Autodiff<Autodiff<_>>`,
    /// PLAN_BACKWARD.md §1.2), verificado en valor/gradiente contra `HullWhite1FGreeksResult`
    /// (mismo modelo, dos implementaciones independientes) y en Hessiano contra bump-and-reval de
    /// segundo orden (ver los tests de `hull_white_dual`).
    struct HullWhite1FHessianResult {
        value: f64,
        d_a: f64,
        d_b: f64,
        d_sigma: f64,
        d_r0: f64,
        d_aa: f64,
        d_bb: f64,
        d_sigmasigma: f64,
        d_r0r0: f64,
        d_ab: f64,
        d_asigma: f64,
        d_ar0: f64,
        d_bsigma: f64,
        d_br0: f64,
        d_sigmar0: f64,
    }

    /// Equivalente de dos factores de `HullWhite1FGreeksResult` -- sin `d_rho` (`rho` no es un
    /// tensor diferenciable en `HullWhite2F`, ver `engine_core::api::HullWhite2FGreeks`).
    struct HullWhite2FGreeksResult {
        d_a: f64,
        d_b: f64,
        d_sigma: f64,
        d_eta: f64,
        d_r0: f64,
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

        // Las cuatro derivadas de irs_hull_white_npv_delta_r0 (a/b/sigma/r0) en una UNICA pasada
        // backward() (PLAN_GREEKS.md §5.2/§11 Fase 7), ver
        // engine_core::api::irs_hull_white_npv_all_greeks. Generaliza el bridge de arriba, que
        // sigue existiendo sin cambios (fachada retrocompatible, PLAN_GREEKS.md §10).
        fn irs_hull_white_npv_all_greeks(
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
        ) -> HullWhite1FGreeksResult;

        // Hessiano cerrado de Hull-White 1F (PLAN_BACKWARD.md §9 Fase 2): valor + gradiente + las
        // 10 entradas del Hessiano 4x4, via Dual2/HyperDual nuevos (forward-over-forward), no via
        // Burn (que no anida Autodiff para un Hessiano, PLAN_BACKWARD.md §1.2). Mismos parámetros
        // que `irs_hull_white_npv_all_greeks` -- ver `engine_core::models::hull_white_dual::
        // hull_white_1f_hessian`.
        fn hull_white_1f_hessian(
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
        ) -> HullWhite1FHessianResult;

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

        // Equivalente 2F de irs_hull_white_npv_all_greeks -- a/b/sigma/eta/r0 en una unica
        // pasada backward() (sin d_rho, ver HullWhite2FGreeksResult).
        fn irs_hull_white_2f_npv_all_greeks(
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
        ) -> HullWhite2FGreeksResult;

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
        //
        // `valuation_time` (PLAN_GREEKS.md §7.2/Fase 5): desplaza "hoy" -- 0.0 preserva el
        // comportamiento previo. Ver `engine_core::payoff::price_payoff_gbm_q`/
        // `simulate_gbm_columns` para el rechazo explicito si cruza un instante requerido.
        fn price_payoff_gbm_q(
            spec_json: String,
            observable: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
            valuation_time: f64,
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

        // PLAN_PRODUCTS.md §10, Fase 9: precio bajo Q (GBM, Longstaff-Schwartz) de un
        // PayoffProgram con exactamente un ContractOp::Exercise, mas el diagnostico de la
        // politica de ejercicio resuelta -- ver engine_core::payoff::price_payoff_exercise_gbm_q.
        // Mismo preflight de observable que price_payoff_gbm_q, mas "el contrato no contiene
        // ningun nodo Exercise"/"mas de un nodo Exercise" (alcance de esta fase, ver
        // engine_core::payoff::lsm).
        #[allow(clippy::too_many_arguments)]
        fn price_payoff_exercise_gbm_q(
            spec_json: String,
            observable: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<ExercisePolicyResult>;

        // PLAN_PRODUCTS.md §12 Fase 6 ("perfil de exposicion pathwise a partir del mismo AST y
        // netting explicito"): EE/PFE95 pathwise de un PayoffProgram bajo Q en cada instante de
        // `exposure_times`, ver engine_core::payoff::payoff_exposure_profile_gbm_q. Mismo
        // preflight de observables que price_payoff_gbm_q; `exposure_times` vacio o con algun
        // instante negativo/no finito tambien es un error de preflight.
        fn payoff_exposure_profile_gbm_q(
            spec_json: String,
            observable: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            exposure_times: Vec<f64>,
            n_paths: u64,
            seed: u64,
        ) -> Result<ExposureProfileResult>;

        // PLAN_PRODUCTS.md §12 Fase 7: "Forecast" bajo P (GBM fisico, drift `mu`) del mismo JSON
        // canonico engine.payoff/v1 -- ver engine_core::payoff::forecast_gbm_p. Preflight identico
        // en espiritu al de price_payoff_gbm_q (observable no generado, n_paths=0), pero NUNCA
        // descuenta (ver el doc-comment de engine_core::payoff::api_p).
        fn forecast_gbm_p(
            spec_json: String,
            observable: String,
            s0: f64,
            mu: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffPForecastResult>;

        // PLAN_PRODUCTS.md §12 Fase 7: probabilidad bajo P de que `event` dispare, sobre las
        // mismas rutas GBM fisico que usaria forecast_gbm_p -- ver
        // engine_core::payoff::hit_probability_gbm_p.
        fn hit_probability_gbm_p(
            spec_json: String,
            event: String,
            observable: String,
            s0: f64,
            mu: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffPHitProbabilityResult>;

        // PLAN_PRODUCTS.md §12 Fase 7: distribucion de P&L de una estrategia bajo P (media, error
        // estandar, VaR y Expected Shortfall al nivel `confidence`) -- ver
        // engine_core::payoff::pnl_distribution_gbm_p. `confidence` fuera de [0,1) es un error de
        // preflight.
        #[allow(clippy::too_many_arguments)]
        fn pnl_distribution_gbm_p(
            spec_json: String,
            observable: String,
            s0: f64,
            mu: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
            confidence: f64,
        ) -> Result<PnlDistributionResult>;

        // PLAN_PRODUCTS.md §12 Fase 11 (item pendiente): sensibilidad pathwise bajo Q de un
        // PayoffProgram respecto de "spot"/"rate"/"dividend_yield"/"volatility" -- ver
        // engine_core::payoff::payoff_sensitivity_gbm_q, que ya decide internamente entre el
        // metodo pathwise (Dual) y el fallback bump-and-reval si el contrato contiene Exercise
        // (ver engine_core::payoff::sensitivity). `greek` fuera de las cuatro cadenas soportadas
        // es un error de preflight.
        #[allow(clippy::too_many_arguments)]
        fn payoff_sensitivity_gbm_q(
            spec_json: String,
            observable: String,
            greek: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffSensitivityResult>;

        // PLAN_GREEKS.md §5.1/§11 Fase 7: extension bajo P de payoff_sensitivity_gbm_q, mismo
        // metodo pathwise (Dual), sin fallback bump-and-reval (GbmP nunca declara soporte de
        // Exercise, ver engine_core::payoff::api_p::payoff_sensitivity_gbm_p) y sin descuento
        // (mismo criterio que forecast_gbm_p). `greek` fuera de "spot"/"mu"/"volatility" es un
        // error de preflight. Reutiliza PayoffSensitivityResult (misma forma, "value" es una
        // derivada bajo P en vez de bajo Q).
        #[allow(clippy::too_many_arguments)]
        fn payoff_sensitivity_gbm_p(
            spec_json: String,
            observable: String,
            greek: String,
            s0: f64,
            mu: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffSensitivityResult>;

        // PLAN_GREEKS.md §5.1/§11 Fase 7: `true` si spec_json contiene al menos un
        // ContractOp::Exercise -- consulta de capacidad que engine::greeks::compute_greek (C++)
        // usa ANTES de intentar pathwise, para reportar GreekResult::method_used correctamente
        // (ver engine_core::payoff::payoff_contains_exercise para el porque).
        fn payoff_contains_exercise(spec_json: String) -> Result<bool>;

        // PLAN_HYPERDUAL.md §5: Gamma ("segunda derivada PURA respecto de 'spot'") bajo Q via el
        // metodo del ratio de verosimilitud (Broadie-Glasserman), ver
        // engine_core::payoff::payoff_sensitivity2_gbm_q -- reemplaza la generalizacion original
        // de este documento (`Dual2`, derivar el PAYOFF dos veces), que resulto matematicamente
        // incorrecta para cualquier payoff con un kink (Max/Min/Abs/If/Trigger) que dependa del
        // parametro derivado, ver el doc-comment de `engine_core::payoff::lrm`. Solo soportado
        // para contratos de una unica fecha terminal (`payoff_supports_second_order_lrm` decide
        // ANTES de llamar aqui) y `greek == "spot"`.
        fn payoff_sensitivity2_gbm_q(
            spec_json: String,
            observable: String,
            greek: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffSensitivityResult>;

        // Extension bajo P de payoff_sensitivity2_gbm_q -- ver engine_core::payoff::payoff_sensitivity2_gbm_p.
        #[allow(clippy::too_many_arguments)]
        fn payoff_sensitivity2_gbm_p(
            spec_json: String,
            observable: String,
            greek: String,
            s0: f64,
            mu: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffSensitivityResult>;

        // PLAN_HYPERDUAL.md §5: Vanna (derivada cruzada "spot"/"volatility") bajo Q via likelihood
        // ratio -- ver engine_core::payoff::payoff_sensitivity_cross_gbm_q. Solo soportado para
        // contratos de una unica fecha terminal y para el par ("spot","volatility") en cualquier
        // orden.
        #[allow(clippy::too_many_arguments)]
        fn payoff_sensitivity_cross_gbm_q(
            spec_json: String,
            observable: String,
            risk_factor: String,
            cross_factor: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffSensitivityResult>;

        // Extension bajo P de payoff_sensitivity_cross_gbm_q -- ver
        // engine_core::payoff::payoff_sensitivity_cross_gbm_p.
        #[allow(clippy::too_many_arguments)]
        fn payoff_sensitivity_cross_gbm_p(
            spec_json: String,
            observable: String,
            risk_factor: String,
            cross_factor: String,
            s0: f64,
            mu: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffSensitivityResult>;

        // PLAN_BACKWARD.md §9 Fase 1: Hessiano local (Gamma/Volga/Vanna) bajo Q via likelihood
        // ratio, ver engine_core::payoff::payoff_local_hessian_gbm_q -- UNA SOLA simulacion GBM
        // reutilizada para las tres salidas (complementa, no sustituye, a
        // payoff_sensitivity2_gbm_q/payoff_sensitivity_cross_gbm_q). Mismo alcance: solo contratos
        // de una unica fecha terminal (`payoff_supports_second_order_lrm` decide ANTES de llamar
        // aqui); no hay parametro `greek` porque siempre se calculan las tres entradas.
        #[allow(clippy::too_many_arguments)]
        fn payoff_local_hessian_gbm_q(
            spec_json: String,
            observable: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffLocalHessianResult>;

        // Extension bajo P de payoff_local_hessian_gbm_q -- ver
        // engine_core::payoff::payoff_local_hessian_gbm_p.
        #[allow(clippy::too_many_arguments)]
        fn payoff_local_hessian_gbm_p(
            spec_json: String,
            observable: String,
            s0: f64,
            mu: f64,
            sigma: f64,
            n_paths: u64,
            seed: u64,
        ) -> Result<PayoffLocalHessianResult>;

        // PLAN_HYPERDUAL.md §5: `true` si spec_json compila y depende del subyacente en una unica
        // fecha terminal -- consulta de capacidad que engine::greeks::compute_greek (C++) usa
        // ANTES de intentar Gamma/Vanna via likelihood ratio, mismo criterio que
        // payoff_contains_exercise.
        fn payoff_supports_second_order_lrm(spec_json: String) -> Result<bool>;

        // Extension bajo P de payoff_supports_second_order_lrm.
        fn payoff_supports_second_order_lrm_p(spec_json: String) -> Result<bool>;

        // PLAN_PRODUCTS.md §11/§12 Fase 11 (item pendiente): sintetiza una cobertura bajo GBM/Q
        // para target_spec_json con el universo instrument_specs_json -- ver
        // engine_core::payoff::synthesize_hedge_gbm_q/HedgeConstraints. Convenciones de
        // "ausente" a traves de esta frontera (cxx no tiene Option nativo para argumentos
        // primitivos): instrument_prices vacio = sin precios; lower_bounds/upper_bounds ambos
        // vacios = sin restriccion de caja (si no vacios, deben tener longitud
        // instrument_specs_json.len() cada uno, con +-INFINITY para "sin limite" en un lado);
        // max_gross_notional < 0.0 = sin limite de liquidez.
        #[allow(clippy::too_many_arguments)]
        fn synthesize_hedge_gbm_q(
            target_spec_json: String,
            instrument_specs_json: Vec<String>,
            observable: String,
            s0: f64,
            r: f64,
            q: f64,
            sigma: f64,
            instrument_prices: Vec<f64>,
            ridge: f64,
            lower_bounds: Vec<f64>,
            upper_bounds: Vec<f64>,
            max_gross_notional: f64,
            compute_residual_greeks: bool,
            n_paths: u64,
            seed: u64,
        ) -> Result<HedgeSynthesisResult>;
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
fn irs_hull_white_npv_all_greeks(
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
) -> ffi::HullWhite1FGreeksResult {
    let greeks = engine_core::api::irs_hull_white_npv_all_greeks(
        a, b, sigma, r0, notional, fixed_rate, use_par_rate, start, payment_times, accruals,
    );
    ffi::HullWhite1FGreeksResult { d_a: greeks.d_a, d_b: greeks.d_b, d_sigma: greeks.d_sigma, d_r0: greeks.d_r0 }
}

#[allow(clippy::too_many_arguments)]
fn hull_white_1f_hessian(
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
) -> ffi::HullWhite1FHessianResult {
    let hessian = engine_core::models::hull_white_dual::hull_white_1f_hessian(
        a, b, sigma, r0, notional, fixed_rate, use_par_rate, start, &payment_times, &accruals,
    );
    ffi::HullWhite1FHessianResult {
        value: hessian.value,
        d_a: hessian.d_a,
        d_b: hessian.d_b,
        d_sigma: hessian.d_sigma,
        d_r0: hessian.d_r0,
        d_aa: hessian.d_aa,
        d_bb: hessian.d_bb,
        d_sigmasigma: hessian.d_sigmasigma,
        d_r0r0: hessian.d_r0r0,
        d_ab: hessian.d_ab,
        d_asigma: hessian.d_asigma,
        d_ar0: hessian.d_ar0,
        d_bsigma: hessian.d_bsigma,
        d_br0: hessian.d_br0,
        d_sigmar0: hessian.d_sigmar0,
    }
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
fn irs_hull_white_2f_npv_all_greeks(
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
) -> ffi::HullWhite2FGreeksResult {
    let greeks = engine_core::api::irs_hull_white_2f_npv_all_greeks(
        a, b, sigma, eta, rho, r0, notional, fixed_rate, use_par_rate, start, payment_times, accruals,
    );
    ffi::HullWhite2FGreeksResult {
        d_a: greeks.d_a,
        d_b: greeks.d_b,
        d_sigma: greeks.d_sigma,
        d_eta: greeks.d_eta,
        d_r0: greeks.d_r0,
    }
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
    valuation_time: f64,
) -> Result<ffi::PayoffQPriceResult, String> {
    let estimate = engine_core::payoff::price_payoff_gbm_q(
        "cpu", &spec_json, &observable, s0, r, q, sigma, n_paths, seed, valuation_time,
    )?;
    Ok(ffi::PayoffQPriceResult {
        mean: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn price_payoff_exercise_gbm_q(
    spec_json: String,
    observable: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::ExercisePolicyResult, String> {
    let result =
        engine_core::payoff::price_payoff_exercise_gbm_q("cpu", &spec_json, &observable, s0, r, q, sigma, n_paths, seed)?;
    let dates = result
        .dates
        .into_iter()
        .map(|d| {
            let (has_regression, coeff_a, coeff_b, coeff_c) = match d.regression_coeffs {
                Some((a, b, c)) => (true, a, b, c),
                None => (false, 0.0, 0.0, 0.0),
            };
            ffi::ExerciseDateDiagnosticResult {
                date: d.date,
                n_in_the_money: d.n_in_the_money,
                has_regression,
                coeff_a,
                coeff_b,
                coeff_c,
                exercised_fraction: d.exercised_fraction,
            }
        })
        .collect();
    Ok(ffi::ExercisePolicyResult {
        mean: result.price.mean,
        std_error: result.price.std_error,
        ci_low: result.price.ci_low,
        ci_high: result.price.ci_high,
        n_paths: result.price.n_paths,
        dates,
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
        engine_core::payoff::hit_probability_gbm_q("cpu", &spec_json, &event, &observable, s0, r, q, sigma, n_paths, seed)?;
    Ok(ffi::PayoffQHitProbabilityResult {
        probability: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_exposure_profile_gbm_q(
    spec_json: String,
    observable: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    exposure_times: Vec<f64>,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::ExposureProfileResult, String> {
    let profile = engine_core::payoff::payoff_exposure_profile_gbm_q(
        "cpu",
        &spec_json,
        &observable,
        s0,
        r,
        q,
        sigma,
        &exposure_times,
        n_paths,
        seed,
    )?;
    Ok(ffi::ExposureProfileResult { times: profile.times, ee: profile.ee, pfe_95: profile.pfe_95 })
}

#[allow(clippy::too_many_arguments)]
fn forecast_gbm_p(
    spec_json: String,
    observable: String,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffPForecastResult, String> {
    let estimate = engine_core::payoff::forecast_gbm_p(&spec_json, &observable, s0, mu, sigma, n_paths, seed)?;
    Ok(ffi::PayoffPForecastResult {
        mean: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn hit_probability_gbm_p(
    spec_json: String,
    event: String,
    observable: String,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffPHitProbabilityResult, String> {
    let estimate =
        engine_core::payoff::hit_probability_gbm_p(&spec_json, &event, &observable, s0, mu, sigma, n_paths, seed)?;
    Ok(ffi::PayoffPHitProbabilityResult {
        probability: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn pnl_distribution_gbm_p(
    spec_json: String,
    observable: String,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
    confidence: f64,
) -> Result<ffi::PnlDistributionResult, String> {
    let dist = engine_core::payoff::pnl_distribution_gbm_p(
        &spec_json, &observable, s0, mu, sigma, n_paths, seed, confidence,
    )?;
    Ok(ffi::PnlDistributionResult {
        mean: dist.mean,
        std_error: dist.std_error,
        var: dist.var,
        es: dist.es,
        n_paths: dist.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity_gbm_q(
    spec_json: String,
    observable: String,
    greek: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffSensitivityResult, String> {
    let estimate = engine_core::payoff::payoff_sensitivity_gbm_q(
        "cpu", &spec_json, &observable, &greek, s0, r, q, sigma, n_paths, seed,
    )?;
    Ok(ffi::PayoffSensitivityResult {
        value: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity_gbm_p(
    spec_json: String,
    observable: String,
    greek: String,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffSensitivityResult, String> {
    let estimate = engine_core::payoff::payoff_sensitivity_gbm_p(&spec_json, &observable, &greek, s0, mu, sigma, n_paths, seed)?;
    Ok(ffi::PayoffSensitivityResult {
        value: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

fn payoff_contains_exercise(spec_json: String) -> Result<bool, String> {
    engine_core::payoff::payoff_contains_exercise(&spec_json)
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity2_gbm_q(
    spec_json: String,
    observable: String,
    greek: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffSensitivityResult, String> {
    let estimate = engine_core::payoff::payoff_sensitivity2_gbm_q(
        "cpu", &spec_json, &observable, &greek, s0, r, q, sigma, n_paths, seed,
    )?;
    Ok(ffi::PayoffSensitivityResult {
        value: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity2_gbm_p(
    spec_json: String,
    observable: String,
    greek: String,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffSensitivityResult, String> {
    let estimate = engine_core::payoff::payoff_sensitivity2_gbm_p(&spec_json, &observable, &greek, s0, mu, sigma, n_paths, seed)?;
    Ok(ffi::PayoffSensitivityResult {
        value: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity_cross_gbm_q(
    spec_json: String,
    observable: String,
    risk_factor: String,
    cross_factor: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffSensitivityResult, String> {
    let estimate = engine_core::payoff::payoff_sensitivity_cross_gbm_q(
        "cpu", &spec_json, &observable, &risk_factor, &cross_factor, s0, r, q, sigma, n_paths, seed,
    )?;
    Ok(ffi::PayoffSensitivityResult {
        value: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity_cross_gbm_p(
    spec_json: String,
    observable: String,
    risk_factor: String,
    cross_factor: String,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffSensitivityResult, String> {
    let estimate = engine_core::payoff::payoff_sensitivity_cross_gbm_p(
        &spec_json, &observable, &risk_factor, &cross_factor, s0, mu, sigma, n_paths, seed,
    )?;
    Ok(ffi::PayoffSensitivityResult {
        value: estimate.mean,
        std_error: estimate.std_error,
        ci_low: estimate.ci_low,
        ci_high: estimate.ci_high,
        n_paths: estimate.n_paths,
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_local_hessian_gbm_q(
    spec_json: String,
    observable: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffLocalHessianResult, String> {
    let estimate = engine_core::payoff::payoff_local_hessian_gbm_q(
        "cpu", &spec_json, &observable, s0, r, q, sigma, n_paths, seed,
    )?;
    Ok(ffi::PayoffLocalHessianResult {
        gamma: ffi::PayoffSensitivityResult {
            value: estimate.gamma.mean,
            std_error: estimate.gamma.std_error,
            ci_low: estimate.gamma.ci_low,
            ci_high: estimate.gamma.ci_high,
            n_paths: estimate.gamma.n_paths,
        },
        volga: ffi::PayoffSensitivityResult {
            value: estimate.volga.mean,
            std_error: estimate.volga.std_error,
            ci_low: estimate.volga.ci_low,
            ci_high: estimate.volga.ci_high,
            n_paths: estimate.volga.n_paths,
        },
        vanna: ffi::PayoffSensitivityResult {
            value: estimate.vanna.mean,
            std_error: estimate.vanna.std_error,
            ci_low: estimate.vanna.ci_low,
            ci_high: estimate.vanna.ci_high,
            n_paths: estimate.vanna.n_paths,
        },
    })
}

#[allow(clippy::too_many_arguments)]
fn payoff_local_hessian_gbm_p(
    spec_json: String,
    observable: String,
    s0: f64,
    mu: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::PayoffLocalHessianResult, String> {
    let estimate = engine_core::payoff::payoff_local_hessian_gbm_p(&spec_json, &observable, s0, mu, sigma, n_paths, seed)?;
    Ok(ffi::PayoffLocalHessianResult {
        gamma: ffi::PayoffSensitivityResult {
            value: estimate.gamma.mean,
            std_error: estimate.gamma.std_error,
            ci_low: estimate.gamma.ci_low,
            ci_high: estimate.gamma.ci_high,
            n_paths: estimate.gamma.n_paths,
        },
        volga: ffi::PayoffSensitivityResult {
            value: estimate.volga.mean,
            std_error: estimate.volga.std_error,
            ci_low: estimate.volga.ci_low,
            ci_high: estimate.volga.ci_high,
            n_paths: estimate.volga.n_paths,
        },
        vanna: ffi::PayoffSensitivityResult {
            value: estimate.vanna.mean,
            std_error: estimate.vanna.std_error,
            ci_low: estimate.vanna.ci_low,
            ci_high: estimate.vanna.ci_high,
            n_paths: estimate.vanna.n_paths,
        },
    })
}

fn payoff_supports_second_order_lrm(spec_json: String) -> Result<bool, String> {
    engine_core::payoff::payoff_supports_second_order_lrm(&spec_json)
}

fn payoff_supports_second_order_lrm_p(spec_json: String) -> Result<bool, String> {
    engine_core::payoff::payoff_supports_second_order_lrm_p(&spec_json)
}

#[allow(clippy::too_many_arguments)]
fn synthesize_hedge_gbm_q(
    target_spec_json: String,
    instrument_specs_json: Vec<String>,
    observable: String,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    instrument_prices: Vec<f64>,
    ridge: f64,
    lower_bounds: Vec<f64>,
    upper_bounds: Vec<f64>,
    max_gross_notional: f64,
    compute_residual_greeks: bool,
    n_paths: u64,
    seed: u64,
) -> Result<ffi::HedgeSynthesisResult, String> {
    let prices = if instrument_prices.is_empty() { None } else { Some(instrument_prices.as_slice()) };
    let bounds = if lower_bounds.is_empty() && upper_bounds.is_empty() {
        None
    } else {
        if lower_bounds.len() != upper_bounds.len() {
            return Err(format!(
                "hedge: lower_bounds tiene {} elementos pero upper_bounds tiene {} -- deben coincidir",
                lower_bounds.len(),
                upper_bounds.len()
            ));
        }
        Some(lower_bounds.into_iter().zip(upper_bounds).collect::<Vec<(f64, f64)>>())
    };
    let constraints = engine_core::payoff::HedgeConstraints {
        bounds,
        max_gross_notional: if max_gross_notional >= 0.0 { Some(max_gross_notional) } else { None },
    };

    let result = engine_core::payoff::synthesize_hedge_gbm_q(
        "cpu",
        &target_spec_json,
        &instrument_specs_json,
        &observable,
        s0,
        r,
        q,
        sigma,
        prices,
        ridge,
        &constraints,
        compute_residual_greeks,
        n_paths,
        seed,
    )?;

    let (has_cost, cost) = match result.cost {
        Some(c) => (true, c),
        None => (false, 0.0),
    };
    let (has_gross_notional, gross_notional) = match result.gross_notional {
        Some(g) => (true, g),
        None => (false, 0.0),
    };
    let (has_residual_greeks, residual_greeks_delta, residual_greeks_rho, residual_greeks_dividend_yield, residual_greeks_vega) =
        match result.residual_greeks {
            Some(g) => (true, g.delta, g.rho, g.dividend_yield, g.vega),
            None => (false, 0.0, 0.0, 0.0, 0.0),
        };

    Ok(ffi::HedgeSynthesisResult {
        weights: result.weights,
        residuals: result.residuals,
        residual_mean: result.residual_mean,
        residual_std: result.residual_std,
        residual_max_abs: result.residual_max_abs,
        has_cost,
        cost,
        has_gross_notional,
        gross_notional,
        has_residual_greeks,
        residual_greeks_delta,
        residual_greeks_rho,
        residual_greeks_dividend_yield,
        residual_greeks_vega,
    })
}
