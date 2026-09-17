//! Orquestacion de precio bajo Q de un `CompiledPayoff` (PLAN_PRODUCTS.md §12 Fase 5): compila
//! el JSON canonico `engine.payoff/v1`, simula bajo GBM el unico observable que el modelo genera
//! en los instantes que el programa necesita (`CompiledPayoff::required_times`), interpreta el
//! IR sobre cada ruta (`payoff::eval::evaluate`), descuenta a valor presente con la `r` constante
//! del modelo y agrega media/error estandar/intervalo de confianza (`crate::mc`).
//!
//! Frontera `f64` pura, mismo espiritu que `crate::api` (la que `engine-ffi` bridgea hoy para
//! IRS/Hull-White) -- a diferencia de ese modulo, esta funcion SI devuelve `Result`: el preflight
//! de "observable no generado" (criterio de aceptacion de Fase 5) es un error real que debe
//! propagarse a quien llama, no una degradacion silenciosa como `crate::api::resolve_backend`.
//! `cxx` traduce un `Result::Err(String)` en una excepcion de C++ en el punto de la llamada (ver
//! el bridge en `rust/crates/engine-ffi/src/lib.rs`), asi que este `Result` cruza la frontera tal
//! cual sin necesitar un tipo de error propio.
//!
//! Generico sobre `Backend` (Fase 11, "vectorizacion CPU/GPU"): cada funcion publica recibe
//! `backend: &str` ("cpu"/"gpu") como primer parametro y despacha, via `resolve_backend`
//! (`crate::backend`), a una funcion interna `_on::<B: Backend<FloatElem = f64>>` -- MISMO
//! patron que `crate::api::irs_hull_white_exposure_profile` para IRS/Hull-White (ver el
//! doc-comment de ese modulo). Solo la SIMULACION de las rutas GBM (`simulate_gbm_columns`/
//! `simulate_gbm_columns_at`) corre en el backend elegido; la interpretacion pathwise del IR
//! del payoff (`payoff::eval::evaluate`) y, en `price_payoff_exercise_gbm_q`, la regresion de
//! Longstaff-Schwartz, siguen siendo codigo escalar en `f64` puro sobre `Vec<f64>` ya
//! materializados desde el tensor -- no hay nada de eso que vectorizar sobre backend.

use crate::backend::{resolve_backend, ComputeBackend, CpuBackend};
use crate::exposure::ExposureProfile;
use crate::mc::{self, McEstimate};
use crate::models::gbm::Gbm;
use crate::payoff::compile::compile;
use crate::payoff::dual::Dual;
use crate::payoff::eval::{evaluate_with_events_seeded, evaluate_with_resolved_states, resolve_trigger_states, ObservablePath};
use crate::payoff::ir::CompiledPayoff;
use crate::payoff::lrm;
use crate::payoff::lsm::{self, ExerciseDateDiagnostic};
use crate::payoff::sensitivity::{contains_exercise, evaluate_dual, GbmDualPath, GbmGreek};
use burn::tensor::backend::Backend;
use burn::tensor::{Tensor, TensorData};

fn scalar<B: Backend>(value: f64, device: &burn::tensor::Device<B>) -> Tensor<B, 1> {
    Tensor::from_data(TensorData::from([value]), device)
}

/// Una unica ruta ya simulada: `times[i]` <-> `values[i]`, un unico observable (slot 0 -- el
/// preflight de `price_payoff_gbm_q` garantiza que `payoff.observable_slots` no tenga mas de un
/// nombre distinto antes de llegar aqui). `sigma` es la volatilidad GBM constante de ese
/// observable -- solo la consulta `eval::resolve_trigger_states` para la correccion de Brownian
/// bridge (§4.2, Fase 6); el resto de esta struct no cambia por su presencia.
struct SinglePath<'a> {
    times: &'a [f64],
    values: &'a [f64],
    sigma: f64,
}

impl ObservablePath for SinglePath<'_> {
    fn value_at(&self, _slot: usize, time: f64) -> f64 {
        let idx = self
            .times
            .iter()
            .position(|&t| (t - time).abs() < 1e-9)
            .unwrap_or_else(|| {
                panic!("payoff: tiempo {time} no simulado por el modelo (times={:?})", self.times)
            });
        self.values[idx]
    }

    fn volatility(&self, _slot: usize) -> f64 {
        self.sigma
    }
}

/// Deriva un `bridge_seed` reproducible y distinto por ruta a partir del `seed` global del
/// Monte Carlo (PLAN_PRODUCTS.md §4.2, Fase 6): el `BridgeRng` de `eval::resolve_trigger_states`
/// es una fuente de aleatoriedad DELIBERADAMENTE independiente del RNG de Burn que genera la
/// propia trayectoria (ver el doc-comment de `payoff::eval`) -- mezclar `seed`/`path_idx` con la
/// constante de Weyl (splitmix64) evita que ambos flujos compartan estado o se correlacionen.
/// `pub(crate)` desde PLAN_IMPROVE_NOTEBOOK.md Fase 3: `payoff::basket_api` reutiliza esta MISMA
/// derivacion de semilla por ruta para el mismo proposito (independencia del RNG de Brownian
/// bridge respecto del RNG de Burn que genera la trayectoria), en vez de duplicarla.
pub(crate) fn bridge_seed_for_path(seed: u64, path_idx: usize) -> u64 {
    seed.wrapping_add((path_idx as u64).wrapping_add(1).wrapping_mul(0x9E37_79B9_7F4A_7C15))
}

/// Preflight comun a `price_payoff_gbm_q`/`hit_probability_gbm_q`: `payoff` no referencia ningun
/// observable distinto de `observable` (el UNICO que este `Gbm` genera, PLAN_PRODUCTS.md §6
/// "`ModelCapabilities::generated_observables`") y `n_paths > 0`. `Err` ANTES de simular una sola
/// ruta -- criterio de aceptacion explicito de Fase 5.
pub(crate) fn check_single_observable(payoff: &CompiledPayoff, observable: &str, n_paths: u64) -> Result<(), String> {
    for used in &payoff.observable_slots {
        if used != observable {
            return Err(format!(
                "payoff: observable '{used}' no generado por este modelo GBM (unico observable soportado: '{observable}')"
            ));
        }
    }
    if n_paths == 0 {
        return Err("payoff: n_paths debe ser > 0".to_string());
    }
    Ok(())
}

/// Simula bajo GBM el unico observable de `payoff` en `times` (ya ordenado/deduplicado/> 0 -- ver
/// las dos llamantes) y devuelve `columns`: `columns[i][path]` es el valor de ese observable en
/// `times[i]` para la ruta `path`. Generica sobre `B` (Fase 11): `device` decide en que backend
/// corre la simulacion, el resto de esta funcion no cambia.
///
/// `valuation_time` (PLAN_GREEKS.md §7.2/Fase 5, aditivo, default `0.0` en todas las llamantes
/// salvo `price_payoff_gbm_q_on`): desplaza el "hoy" de la simulacion -- se simula en
/// `times[i] - valuation_time` en vez de `times[i]`, manteniendo `s0` (el spot conocido HOY,
/// theta puro no lo cambia) y el resto de parametros de `Gbm` intactos. La llamante
/// (`simulate_gbm_columns`) es responsable de garantizar `times[i] > valuation_time` para todo
/// `i` -- `Gbm::simulate_at_times` asume tiempos estrictamente positivos.
#[allow(clippy::too_many_arguments)]
fn simulate_gbm_columns_at<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    times: &[f64],
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
    valuation_time: f64,
) -> Vec<Vec<f64>> {
    B::seed(device, seed);
    let shifted_times: Vec<f64> = times.iter().map(|t| t - valuation_time).collect();
    let model = Gbm::<B>::new(scalar(s0, device), scalar(r, device), scalar(q, device), scalar(sigma, device));
    let simulated = model.simulate_at_times(&shifted_times, n_paths as usize, device);
    simulated.into_iter().map(|t| t.into_data().to_vec::<f64>().unwrap()).collect()
}

/// Simula bajo GBM el unico observable de `payoff` en `payoff.required_times()` y devuelve
/// `(times, columns)`: `columns[i][path]` es el valor de ese observable en `times[i]` para la
/// ruta `path` (`times` en su escala ORIGINAL, sin desplazar -- el desplazamiento por
/// `valuation_time` es un detalle interno de la simulacion, el resto del pipeline sigue
/// razonando en tiempos absolutos). Compartido por `price_payoff_gbm_q_on`/
/// `hit_probability_gbm_q_on`/`price_payoff_exercise_gbm_q_on`/`payoff_sensitivity_pathwise_on`.
///
/// `valuation_time` (PLAN_GREEKS.md §7.2/Fase 5): `Err` explicito si algun `required_time` ya
/// ocurrio en o antes de `valuation_time` -- este motor no modela un `FixingStore` de historico
/// para la ruta simulada bajo Q (a diferencia de la ruta determinista `scenario_payoff`/
/// `present_value`, ver PLAN_GREEKS.md §7.4), asi que Theta nunca puede cruzar un instante
/// requerido por el contrato: se rechaza explicito, nunca se aproxima en silencio.
#[allow(clippy::too_many_arguments)]
fn simulate_gbm_columns<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    payoff: &CompiledPayoff,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
    valuation_time: f64,
) -> Result<(Vec<f64>, Vec<Vec<f64>>), String> {
    let times = payoff.required_times();
    if times.is_empty() {
        return Err(
            "payoff: el contrato no depende de ningun instante de mercado (nada que simular bajo Q)".to_string(),
        );
    }
    if let Some(&past) = times.iter().find(|&&t| t <= valuation_time) {
        return Err(format!(
            "payoff: valuation_time ({valuation_time}) alcanza o supera un instante requerido por el contrato \
             ({past}) -- este motor no modela fixings historicos de la ruta Monte Carlo bajo Q \
             (PLAN_GREEKS.md §7.4), Theta no puede cruzar ese instante"
        ));
    }
    let columns = simulate_gbm_columns_at::<B>(device, &times, s0, r, q, sigma, n_paths, seed, valuation_time);
    Ok((times, columns))
}

/// Precio bajo Q (`E_Q[cashflows descontados]`) de un `PayoffProgram` serializado en
/// `spec_json` (`engine.payoff/v1`) bajo GBM (`s0`/`r`/`q`/`sigma`), mas error estandar e
/// intervalo de confianza del estimador Monte Carlo (`crate::mc::McEstimate`).
///
/// `observable` es el UNICO nombre de observable que este `Gbm` genera (PLAN_PRODUCTS.md §6,
/// "`ModelCapabilities::generated_observables`"): si el contrato referencia cualquier otro
/// nombre, o cualquier nodo no soportado en `CompiledPayoff` v1 (Fase 5), `Err` ANTES de simular
/// una sola ruta -- preflight, criterio de aceptacion explicito de Fase 5.
///
/// `valuation_time` (PLAN_GREEKS.md §7.2/Fase 5 de PLAN_GREEKS.md, NO confundir con la "Fase 5"
/// de PLAN_PRODUCTS.md citada arriba): desplaza "hoy" -- aditivo, default `0.0` en cada llamante
/// C++ existente (`engine::payoff::risk_neutral_price_gbm`, ver measures.hpp). Theta puro
/// (`compute_greek`, `RiskFactorKind::TimeShift`) revalora con `valuation_time = dt` manteniendo
/// `s0`/`r`/`q`/`sigma` intactos; ver `simulate_gbm_columns` para el rechazo explicito si `dt`
/// cruza un instante requerido por el contrato.
#[allow(clippy::too_many_arguments)]
pub fn price_payoff_gbm_q(
    backend: &str,
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
    valuation_time: f64,
) -> Result<McEstimate, String> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            price_payoff_gbm_q_on::<CpuBackend>(&device, spec_json, observable, s0, r, q, sigma, n_paths, seed, valuation_time)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                price_payoff_gbm_q_on::<crate::backend::GpuBackend>(
                    &device, spec_json, observable, s0, r, q, sigma, n_paths, seed, valuation_time,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                // Inalcanzable en la practica: `resolve_backend` ya cae a `Cpu` cuando este
                // build no tiene la feature `gpu`. CPU como red de seguridad, no como
                // comportamiento normal (ver crate::api::irs_hull_white_exposure_profile).
                let device = burn::tensor::Device::<CpuBackend>::default();
                price_payoff_gbm_q_on::<CpuBackend>(&device, spec_json, observable, s0, r, q, sigma, n_paths, seed, valuation_time)
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn price_payoff_gbm_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
    valuation_time: f64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let (times, columns) = simulate_gbm_columns::<B>(device, &payoff, s0, r, q, sigma, n_paths, seed, valuation_time)?;
    let n_paths_usize = n_paths as usize;

    let mut discounted_samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &times, values: &values, sigma };
        let (ledger, _events) =
            evaluate_with_events_seeded(&payoff, &path, bridge_seed_for_path(seed, path_idx));
        let present_value: f64 =
            ledger.iter().map(|cf| cf.amount * (-r * (cf.payment_time - valuation_time)).exp()).sum();
        discounted_samples.push(present_value);
    }

    Ok(mc::aggregate(&discounted_samples, mc::Z_95))
}

/// Precio bajo Q de un programa con exactamente un `ContractOp::Exercise` mas el diagnostico de la
/// politica de ejercicio que produjo ese precio (PLAN_PRODUCTS.md §10, Fase 9): "politica de
/// ejercicio exportable", criterio de aceptacion explicito de esta fase. `price` tiene la misma
/// forma que el resultado de `price_payoff_gbm_q` (media/error estandar/intervalo de confianza del
/// estimador Monte Carlo); `dates` documenta, por cada fecha de decision (orden ascendente), cuantas
/// rutas estaban in-the-money, los coeficientes de la regresion de continuacion ajustada (si pudo
/// ajustarse) y que fraccion de esas rutas ejercito en esa fecha -- ver `lsm::ExerciseDateDiagnostic`.
#[derive(Debug)]
pub struct ExercisePolicyResult {
    pub price: McEstimate,
    pub dates: Vec<ExerciseDateDiagnostic>,
}

/// Precio bajo Q (Monte Carlo, GBM, Longstaff-Schwartz) de un `PayoffProgram` con un derecho de
/// ejercicio americano/bermuda (PLAN_PRODUCTS.md §10, Fase 9). Mismos parametros/preflight de
/// observable que `price_payoff_gbm_q`, mas `lsm::find_single_exercise_node` (el contrato debe
/// contener EXACTAMENTE un `ContractOp::Exercise`, ver el doc-comment de `lsm` para el porque de
/// esa restriccion) -- todo ANTES de simular una sola ruta.
///
/// Dos pasadas sobre las mismas `n_paths` rutas GBM: (1) `lsm::resolve_exercise_decisions` decide,
/// por Longstaff-Schwartz, en que fecha (si alguna) ejercita cada ruta; (2) cada ruta se
/// reinterpreta con esa decision ya fijada (`eval::evaluate_with_resolved_states`, que APLICA la
/// decision sin volver a tomarla -- ver el doc-comment de `ContractOp::Exercise` en `eval`) y se
/// descuenta/agrega igual que `price_payoff_gbm_q`. Determinista dado `seed` (ninguna de las dos
/// pasadas consume aleatoriedad propia mas alla de la simulacion GBM inicial): dos llamadas con el
/// mismo `seed` producen el mismo precio y el mismo diagnostico -- "decisiones reproducibles con
/// seed", criterio de aceptacion de esta fase.
#[allow(clippy::too_many_arguments)]
pub fn price_payoff_exercise_gbm_q(
    backend: &str,
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ExercisePolicyResult, String> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            price_payoff_exercise_gbm_q_on::<CpuBackend>(&device, spec_json, observable, s0, r, q, sigma, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                price_payoff_exercise_gbm_q_on::<crate::backend::GpuBackend>(
                    &device, spec_json, observable, s0, r, q, sigma, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                price_payoff_exercise_gbm_q_on::<CpuBackend>(
                    &device, spec_json, observable, s0, r, q, sigma, n_paths, seed,
                )
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn price_payoff_exercise_gbm_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<ExercisePolicyResult, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let node = lsm::find_single_exercise_node(&payoff)?;
    // valuation_time=0.0: Theta de un contrato con Exercise queda fuera de esta fase (Fase 5 de
    // PLAN_GREEKS.md solo cablea price_payoff_gbm_q) -- aditivo, comportamiento sin cambios.
    let (times, columns) = simulate_gbm_columns::<B>(device, &payoff, s0, r, q, sigma, n_paths, seed, 0.0)?;
    let n_paths_usize = n_paths as usize;

    // Materializar los valores de TODAS las rutas antes de construir los `SinglePath` (que solo
    // guardan referencias): a diferencia de `price_payoff_gbm_q`, aqui hace falta tener todas las
    // rutas vivas SIMULTANEAMENTE (la regresion de cada fecha es transversal sobre el lote
    // completo, no se puede procesar ruta a ruta de forma independiente).
    let all_values: Vec<Vec<f64>> =
        (0..n_paths_usize).map(|path_idx| columns.iter().map(|col| col[path_idx]).collect()).collect();
    let paths: Vec<SinglePath> = all_values.iter().map(|values| SinglePath { times: &times, values, sigma }).collect();

    let trigger_states_by_path: Vec<_> = paths
        .iter()
        .enumerate()
        .map(|(path_idx, path)| resolve_trigger_states(&payoff, path, bridge_seed_for_path(seed, path_idx)))
        .collect();

    let (decisions, diagnostics) =
        lsm::resolve_exercise_decisions(&payoff, &node, &paths, &trigger_states_by_path, r);

    let mut discounted_samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let mut states = trigger_states_by_path[path_idx].clone();
        states[node.event].occurred = decisions[path_idx].is_some();
        states[node.event].first_hit_time = decisions[path_idx];
        let ledger = evaluate_with_resolved_states(&payoff, &paths[path_idx], &states);
        let present_value: f64 = ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum();
        discounted_samples.push(present_value);
    }

    Ok(ExercisePolicyResult { price: mc::aggregate(&discounted_samples, mc::Z_95), dates: diagnostics })
}

/// Sensibilidad ("Greek") de `price_payoff_gbm_q` respecto de uno de los cuatro parametros de
/// `Gbm` (`"spot"`, `"rate"`, `"dividend_yield"`, `"volatility"`) -- PLAN_PRODUCTS.md §12 Fase 11,
/// item pendiente "AAD con fallback a bump-and-reval para sensibilidades del pricer Monte Carlo
/// GBM de payoff".
///
/// Sin `ContractOp::Exercise` en el contrato: pasada pathwise (`crate::payoff::sensitivity`) sobre
/// las MISMAS rutas que usaria `price_payoff_gbm_q` -- una derivada exacta por ruta (sin ruido de
/// discretizacion de un bump finito), agregada igual que cualquier otro estimador Monte Carlo
/// (`crate::mc::McEstimate`). Con `ContractOp::Exercise`: el pathwise method no aplica limpiamente
/// (re-decidir la politica de Longstaff-Schwartz bajo el parametro perturbado cambia la regresion
/// completa, no solo esta ruta) -- fallback automatico a diferencia central bump-and-reval con
/// numeros aleatorios comunes (mismo `seed` en ambas valoraciones bumped, ver
/// `payoff_sensitivity_bump_and_reval`), exactamente el fallback que PLAN_PRODUCTS.md preveia.
#[allow(clippy::too_many_arguments)]
pub fn payoff_sensitivity_gbm_q(
    backend: &str,
    spec_json: &str,
    observable: &str,
    greek: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    if contains_exercise(&payoff) {
        return payoff_sensitivity_bump_and_reval(backend, spec_json, observable, greek, s0, r, q, sigma, n_paths, seed);
    }
    let greek_kind = GbmGreek::parse(greek)?;
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            payoff_sensitivity_pathwise_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, greek_kind, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                payoff_sensitivity_pathwise_on::<crate::backend::GpuBackend>(
                    &device, &payoff, s0, r, q, sigma, greek_kind, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                payoff_sensitivity_pathwise_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, greek_kind, n_paths, seed)
            }
        }
    }
}

/// Pasada pathwise (ver el doc-comment de `payoff_sensitivity_gbm_q` y de `crate::payoff::
/// sensitivity`): simula las mismas rutas GBM que `price_payoff_gbm_q_on`, resuelve los
/// `Trigger`/estados sobre la ruta `f64` (fijos para la derivada, ver `sensitivity`), interpreta el
/// ledger como `Dual` sobre `GbmDualPath` y descuenta con un `r` TAMBIEN dual
/// (`GbmDualPath::rate_dual`) -- si `greek == Rate`, el propio factor de descuento aporta a la
/// sensibilidad ademas de la ruta, tal como exige `rho`.
#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity_pathwise_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    payoff: &CompiledPayoff,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    greek: GbmGreek,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    // valuation_time=0.0: sensibilidad pathwise queda fuera de esta fase (aditivo, sin cambios).
    let (times, columns) = simulate_gbm_columns::<B>(device, payoff, s0, r, q, sigma, n_paths, seed, 0.0)?;
    let n_paths_usize = n_paths as usize;

    let mut samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let f64_path = SinglePath { times: &times, values: &values, sigma };
        let states = resolve_trigger_states(payoff, &f64_path, bridge_seed_for_path(seed, path_idx));
        let dual_path = GbmDualPath::<Dual>::new(&times, &values, s0, r, q, sigma, greek);
        let ledger = evaluate_dual(payoff, &dual_path, &f64_path, &states);

        let r_dual = dual_path.rate_dual();
        let mut present_value = Dual::constant(0.0);
        for cf in &ledger {
            let discount = (-r_dual * Dual::constant(cf.payment_time)).exp();
            present_value = present_value + cf.amount * discount;
        }
        samples.push(present_value.deriv);
    }

    Ok(mc::aggregate(&samples, mc::Z_95))
}

/// Gamma pathwise EXACTA (segunda derivada PURA respecto de "spot") de `price_payoff_gbm_q`, via
/// el metodo del ratio de verosimilitud (`crate::payoff::lrm`, PLAN_HYPERDUAL.md §5 -- revision:
/// la generalizacion original con `Dual2` resulto matematicamente incorrecta para payoffs con kink,
/// ver el doc-comment de `lrm`). Solo soportado para contratos de una UNICA fecha terminal
/// (`lrm::single_terminal_time`) y para `greek == "spot"`; cualquier otro caso (payoff
/// path-dependiente, `Exercise`, u otro parametro) se rechaza explicito -- `compute_greek` (C++)
/// cae a su estencil generico de bump-and-reval de 3 puntos para esos casos, no hay aproximacion
/// silenciosa aqui.
#[allow(clippy::too_many_arguments)]
pub fn payoff_sensitivity2_gbm_q(
    backend: &str,
    spec_json: &str,
    observable: &str,
    greek: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    lrm::single_terminal_time(&payoff)?;
    if GbmGreek::parse(greek)? != GbmGreek::Spot {
        return Err(format!(
            "payoff: Gamma via likelihood ratio solo soportada para 'spot' (recibido '{greek}') -- \
             PLAN_HYPERDUAL.md §5"
        ));
    }
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            payoff_sensitivity2_lrm_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                payoff_sensitivity2_lrm_on::<crate::backend::GpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                payoff_sensitivity2_lrm_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
            }
        }
    }
}

/// Vanna pathwise EXACTA (derivada cruzada "spot"/"volatility") de `price_payoff_gbm_q`, mismo
/// mecanismo/alcance que `payoff_sensitivity2_gbm_q` (ver su doc-comment): solo contratos de una
/// unica fecha terminal, solo el par `("spot","volatility")` (en cualquier orden).
#[allow(clippy::too_many_arguments)]
pub fn payoff_sensitivity_cross_gbm_q(
    backend: &str,
    spec_json: &str,
    observable: &str,
    risk_factor: &str,
    cross_factor: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    lrm::single_terminal_time(&payoff)?;
    let is_vanna_pair = matches!(
        (GbmGreek::parse(risk_factor)?, GbmGreek::parse(cross_factor)?),
        (GbmGreek::Spot, GbmGreek::Volatility) | (GbmGreek::Volatility, GbmGreek::Spot)
    );
    if !is_vanna_pair {
        return Err(format!(
            "payoff: derivada cruzada via likelihood ratio solo soportada para el par ('spot', \
             'volatility') (Vanna) -- recibido ('{risk_factor}', '{cross_factor}'), PLAN_HYPERDUAL.md §5"
        ));
    }
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            payoff_sensitivity_cross_lrm_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                payoff_sensitivity_cross_lrm_on::<crate::backend::GpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                payoff_sensitivity_cross_lrm_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
            }
        }
    }
}

/// Ledger presente (descontado) de una unica ruta terminal ya simulada -- compartido por
/// `payoff_sensitivity2_lrm_on`/`payoff_sensitivity_cross_lrm_on` (la unica diferencia entre Gamma
/// y Vanna es el peso que multiplica este valor, ver `lrm::gamma_weight`/`lrm::vanna_weight`).
fn discounted_present_value_at_terminal(
    payoff: &CompiledPayoff,
    t: f64,
    s_t: f64,
    r: f64,
    sigma: f64,
    seed: u64,
    path_idx: usize,
) -> f64 {
    let times = [t];
    let values = [s_t];
    let f64_path = SinglePath { times: &times, values: &values, sigma };
    let (ledger, _events) = evaluate_with_events_seeded(payoff, &f64_path, bridge_seed_for_path(seed, path_idx));
    ledger.iter().map(|cf| cf.amount * (-r * cf.payment_time).exp()).sum()
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity2_lrm_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    payoff: &CompiledPayoff,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let (times, columns) = simulate_gbm_columns::<B>(device, payoff, s0, r, q, sigma, n_paths, seed, 0.0)?;
    let t = times[0]; // lrm::single_terminal_time ya garantizo una unica fecha antes de llegar aqui
    let n_paths_usize = n_paths as usize;

    let mut samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let s_t = columns[0][path_idx];
        let present_value = discounted_present_value_at_terminal(payoff, t, s_t, r, sigma, seed, path_idx);
        let z = lrm::recover_terminal_z(s0, r, q, sigma, t, s_t);
        samples.push(present_value * lrm::gamma_weight(z, s0, sigma, t));
    }

    Ok(mc::aggregate(&samples, mc::Z_95))
}

#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity_cross_lrm_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    payoff: &CompiledPayoff,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let (times, columns) = simulate_gbm_columns::<B>(device, payoff, s0, r, q, sigma, n_paths, seed, 0.0)?;
    let t = times[0];
    let n_paths_usize = n_paths as usize;

    let mut samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let s_t = columns[0][path_idx];
        let present_value = discounted_present_value_at_terminal(payoff, t, s_t, r, sigma, seed, path_idx);
        let z = lrm::recover_terminal_z(s0, r, q, sigma, t, s_t);
        samples.push(present_value * lrm::vanna_weight(z, s0, sigma, t));
    }

    Ok(mc::aggregate(&samples, mc::Z_95))
}

/// Resultado de `payoff_local_hessian_gbm_q`/`_p` (PLAN_BACKWARD.md §9 Fase 1): las tres entradas
/// del Hessiano local de una call/put de una UNICA fecha terminal -- `gamma` (`d2V/ds0^2`), `volga`
/// (`d2V/dsigma^2`) y `vanna` (`d2V/(ds0 dsigma)`) -- cada una agregada por separado
/// (`mc::aggregate`) pero sobre la MISMA tanda de rutas simuladas (ver
/// `payoff_local_hessian_lrm_on`). A diferencia de llamar a `payoff_sensitivity2_gbm_q` +
/// `payoff_sensitivity_cross_gbm_q` por separado (dos simulaciones GBM independientes, aunque con
/// el mismo seed), aqui solo hay una: el coste de pedir las 3 entradas es el mismo que pedir 1 sola.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LocalHessianEstimate {
    pub gamma: McEstimate,
    pub volga: McEstimate,
    pub vanna: McEstimate,
}

/// Hessiano local (Gamma/Volga/Vanna) de `price_payoff_gbm_q` via likelihood ratio, en UNA SOLA
/// tanda de rutas simuladas (PLAN_BACKWARD.md §9 Fase 1 -- "una pasada cara, muchas derivadas
/// baratas", la propiedad que motiva todo el documento). Complementa a `payoff_sensitivity2_gbm_q`/
/// `payoff_sensitivity_cross_gbm_q` (que siguen intactas, cada una con su propia simulacion
/// independiente) sin sustituirlas -- ambas rutas coexisten hasta que los llamantes migren a esta
/// (PLAN_BACKWARD.md §9 Fase 1). Mismo alcance que esas dos funciones: solo contratos de una unica
/// fecha terminal (`lrm::single_terminal_time`); no hay parametro `greek` porque siempre se calculan
/// las tres entradas.
#[allow(clippy::too_many_arguments)]
pub fn payoff_local_hessian_gbm_q(
    backend: &str,
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<LocalHessianEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    lrm::single_terminal_time(&payoff)?;
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            payoff_local_hessian_lrm_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                payoff_local_hessian_lrm_on::<crate::backend::GpuBackend>(
                    &device, &payoff, s0, r, q, sigma, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                payoff_local_hessian_lrm_on::<CpuBackend>(&device, &payoff, s0, r, q, sigma, n_paths, seed)
            }
        }
    }
}

/// UNA sola llamada a `simulate_gbm_columns` (verificable por inspeccion: no hay ninguna otra en
/// esta funcion) reutilizada para las tres salidas -- mismo bucle por ruta que
/// `payoff_sensitivity2_lrm_on`/`payoff_sensitivity_cross_lrm_on`, acumulando tres vectores de
/// muestras (`gamma`/`volga`/`vanna`) a partir del MISMO `present_value`/`z` por ruta
/// (`lrm::local_hessian_weights`).
#[allow(clippy::too_many_arguments)]
fn payoff_local_hessian_lrm_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    payoff: &CompiledPayoff,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<LocalHessianEstimate, String> {
    let (times, columns) = simulate_gbm_columns::<B>(device, payoff, s0, r, q, sigma, n_paths, seed, 0.0)?;
    let t = times[0]; // lrm::single_terminal_time ya garantizo una unica fecha antes de llegar aqui
    let n_paths_usize = n_paths as usize;

    let mut gamma_samples = Vec::with_capacity(n_paths_usize);
    let mut volga_samples = Vec::with_capacity(n_paths_usize);
    let mut vanna_samples = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let s_t = columns[0][path_idx];
        let present_value = discounted_present_value_at_terminal(payoff, t, s_t, r, sigma, seed, path_idx);
        let z = lrm::recover_terminal_z(s0, r, q, sigma, t, s_t);
        let weights = lrm::local_hessian_weights(z, s0, sigma, t);
        gamma_samples.push(present_value * weights.gamma);
        volga_samples.push(present_value * weights.volga);
        vanna_samples.push(present_value * weights.vanna);
    }

    Ok(LocalHessianEstimate {
        gamma: mc::aggregate(&gamma_samples, mc::Z_95),
        volga: mc::aggregate(&volga_samples, mc::Z_95),
        vanna: mc::aggregate(&vanna_samples, mc::Z_95),
    })
}

/// Fallback bump-and-reval (diferencia central, numeros aleatorios comunes) para contratos con
/// `ContractOp::Exercise` -- ver el doc-comment de `payoff_sensitivity_gbm_q`. `h` es un bump
/// relativo (`1e-4`), con un piso absoluto (`1e-6`) para el caso de un parametro nominalmente cero
/// (p.ej. `q=0`); ambas valoraciones bumped comparten `seed` para que la unica diferencia entre
/// ellas sea el parametro perturbado, reduciendo varianza frente a semillas independientes.
#[allow(clippy::too_many_arguments)]
fn payoff_sensitivity_bump_and_reval(
    backend: &str,
    spec_json: &str,
    observable: &str,
    greek: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    // 1% relativo (convencion habitual de bump-and-reval Monte Carlo, no el 1e-6 de
    // `tests/aad_vs_bump_reval.rs`): un bump demasiado pequeno divide la diferencia de dos
    // precios Monte Carlo (cada uno con su propio error estandar) por un `denominator` diminuto,
    // amplificando el ruido en vez de acercarse a la derivada -- a diferencia de una formula
    // cerrada (sin ruido de muestreo), aqui el bump debe ser lo bastante grande para que la
    // SENAL (diferencia de precio real) domine sobre el error estandar del estimador.
    const RELATIVE_BUMP: f64 = 1e-2;
    const MIN_ABSOLUTE_BUMP: f64 = 1e-4;
    let bump = |v: f64| (RELATIVE_BUMP * v.abs()).max(MIN_ABSOLUTE_BUMP);

    let greek_kind = GbmGreek::parse(greek)?;
    let (s0_up, s0_dn, r_up, r_dn, q_up, q_dn, sigma_up, sigma_dn, half_denominator) = match greek_kind {
        GbmGreek::Spot => {
            let h = bump(s0);
            (s0 + h, s0 - h, r, r, q, q, sigma, sigma, h)
        }
        GbmGreek::Rate => {
            let h = bump(r);
            (s0, s0, r + h, r - h, q, q, sigma, sigma, h)
        }
        GbmGreek::DividendYield => {
            let h = bump(q);
            (s0, s0, r, r, q + h, q - h, sigma, sigma, h)
        }
        GbmGreek::Volatility => {
            let h = bump(sigma);
            (s0, s0, r, r, q, q, sigma + h, sigma - h, h)
        }
    };

    let up = price_payoff_exercise_gbm_q(backend, spec_json, observable, s0_up, r_up, q_up, sigma_up, n_paths, seed)?.price;
    let down =
        price_payoff_exercise_gbm_q(backend, spec_json, observable, s0_dn, r_dn, q_dn, sigma_dn, n_paths, seed)?.price;

    let denominator = 2.0 * half_denominator;
    let mean = (up.mean - down.mean) / denominator;
    let std_error = (up.std_error.powi(2) + down.std_error.powi(2)).sqrt() / denominator;
    Ok(McEstimate { mean, std_error, ci_low: mean - mc::Z_95 * std_error, ci_high: mean + mc::Z_95 * std_error, n_paths })
}

/// Probabilidad bajo Q de que el evento `event` (un `Trigger` de `spec_json`, identificado por su
/// `EventId`) dispare en la ruta, estimada por Monte Carlo (PLAN_PRODUCTS.md §12 Fase 6: "hit
/// probability Q como medida separada del PV") -- media (la propia probabilidad, en `[0,1]`),
/// error estandar e intervalo de confianza de `crate::mc::McEstimate`, agregando el indicador
/// `1.0`/`0.0` de "el evento ocurrio en esta ruta" sobre las mismas rutas que usaria
/// `price_payoff_gbm_q` para el mismo `spec_json`/modelo (misma simulacion, agregacion distinta:
/// nunca se descuenta un indicador de hit, a diferencia de un cashflow).
#[allow(clippy::too_many_arguments)]
pub fn hit_probability_gbm_q(
    backend: &str,
    spec_json: &str,
    event: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            hit_probability_gbm_q_on::<CpuBackend>(&device, spec_json, event, observable, s0, r, q, sigma, n_paths, seed)
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                hit_probability_gbm_q_on::<crate::backend::GpuBackend>(
                    &device, spec_json, event, observable, s0, r, q, sigma, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                hit_probability_gbm_q_on::<CpuBackend>(
                    &device, spec_json, event, observable, s0, r, q, sigma, n_paths, seed,
                )
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn hit_probability_gbm_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    spec_json: &str,
    event: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    n_paths: u64,
    seed: u64,
) -> Result<McEstimate, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    let event_slot = payoff.event_slots.iter().position(|e| e == event).ok_or_else(|| {
        format!(
            "payoff: el evento '{event}' no existe en este contrato (eventos declarados: {:?})",
            payoff.event_slots
        )
    })?;
    // valuation_time=0.0: Theta de hit probability queda fuera de esta fase (aditivo, sin cambios).
    let (times, columns) = simulate_gbm_columns::<B>(device, &payoff, s0, r, q, sigma, n_paths, seed, 0.0)?;
    let n_paths_usize = n_paths as usize;

    let mut hit_indicators = Vec::with_capacity(n_paths_usize);
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &times, values: &values, sigma };
        let (_ledger, events) =
            evaluate_with_events_seeded(&payoff, &path, bridge_seed_for_path(seed, path_idx));
        hit_indicators.push(if events[event_slot].occurred { 1.0 } else { 0.0 });
    }

    Ok(mc::aggregate(&hit_indicators, mc::Z_95))
}

/// Perfil de exposicion PATHWISE de un `PayoffProgram` bajo GBM (PLAN_PRODUCTS.md §12 Fase 6:
/// "perfil de exposicion pathwise a partir del mismo AST y netting explicito"). Para cada `t` en
/// `exposure_times`, y por cada ruta simulada, `V_t(ruta)` es la suma de
/// `amount * exp(-r*(payment_time menos t))` sobre los cashflows del ledger de ESA ruta con
/// `payment_time >= t` -- el valor REALIZADO restante en esa misma ruta ya simulada, descontado
/// desde `t` (no desde el instante de valoracion). `EE(t) = media(max(V_t, 0))`, `PFE95(t) =
/// percentil 95 de max(V_t, 0)`, mismo tipo `crate::exposure::ExposureProfile` que ya usa la ruta
/// legacy IRS+Hull-White (§5.5: reutilizar el tipo de resultado, no inventar uno nuevo).
///
/// **Deliberadamente no es una valoracion condicional/anidada**: `V_t` usa el UNICO futuro que
/// esa ruta ya realizo (el mismo ledger que produce `evaluate_with_events`), no
/// `E_Q[V_t | F_t]` via regresion (eso es Longstaff-Schwartz, Fase 9, y solo para `Exercise`).
/// Es una simplificacion deliberada y documentada, no una aproximacion oculta (PLAN_PRODUCTS.md
/// §16, ultimo punto: "nunca ocultar aproximaciones").
///
/// **Netting**: si `spec_json` es un portfolio (`Both(trade_1, trade_2, ...)`), el ledger que
/// produce `evaluate_with_events` ya combina los cashflows de TODOS los trades sobre la MISMA
/// ruta -- el netting es automatico por construccion del AST, no un paso aparte (§11).
///
/// `exposure_times` no necesita ser subconjunto de `payoff.required_times()`: se simulan ademas
/// (GBM puede generarse en cualquier instante > 0); un `0.0` en `exposure_times` no se simula
/// (`Gbm::simulate_at_times` exige tiempos > 0, `S0` en `t=0` ya es conocido) -- se resuelve con
/// el ledger ya simulado en los demas tiempos, igual que cualquier otro `t`.
#[allow(clippy::too_many_arguments)]
pub fn payoff_exposure_profile_gbm_q(
    backend: &str,
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    exposure_times: &[f64],
    n_paths: u64,
    seed: u64,
) -> Result<ExposureProfile, String> {
    match resolve_backend(backend) {
        ComputeBackend::Cpu => {
            let device = burn::tensor::Device::<CpuBackend>::default();
            payoff_exposure_profile_gbm_q_on::<CpuBackend>(
                &device, spec_json, observable, s0, r, q, sigma, exposure_times, n_paths, seed,
            )
        }
        ComputeBackend::Gpu => {
            #[cfg(feature = "gpu")]
            {
                let device = burn::tensor::Device::<crate::backend::GpuBackend>::default();
                payoff_exposure_profile_gbm_q_on::<crate::backend::GpuBackend>(
                    &device, spec_json, observable, s0, r, q, sigma, exposure_times, n_paths, seed,
                )
            }
            #[cfg(not(feature = "gpu"))]
            {
                let device = burn::tensor::Device::<CpuBackend>::default();
                payoff_exposure_profile_gbm_q_on::<CpuBackend>(
                    &device, spec_json, observable, s0, r, q, sigma, exposure_times, n_paths, seed,
                )
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn payoff_exposure_profile_gbm_q_on<B: Backend<FloatElem = f64>>(
    device: &burn::tensor::Device<B>,
    spec_json: &str,
    observable: &str,
    s0: f64,
    r: f64,
    q: f64,
    sigma: f64,
    exposure_times: &[f64],
    n_paths: u64,
    seed: u64,
) -> Result<ExposureProfile, String> {
    let payoff = compile(spec_json)?;
    check_single_observable(&payoff, observable, n_paths)?;
    if exposure_times.is_empty() {
        return Err("payoff: exposure_times no puede estar vacio".to_string());
    }
    if exposure_times.iter().any(|t| !t.is_finite() || *t < 0.0) {
        return Err("payoff: exposure_times debe contener solo instantes finitos >= 0".to_string());
    }

    let mut simulation_times: Vec<f64> = payoff.required_times();
    simulation_times.extend(exposure_times.iter().copied().filter(|&t| t > 0.0));
    simulation_times.sort_by(|a, b| a.partial_cmp(b).expect("payoff: tiempo no finito"));
    simulation_times.dedup_by(|a, b| (*a - *b).abs() < 1e-9);
    if simulation_times.is_empty() {
        return Err(
            "payoff: el contrato no depende de ningun instante de mercado (nada que simular bajo Q)".to_string(),
        );
    }

    let n_paths_usize = n_paths as usize;
    // valuation_time=0.0: Theta de exposure profile queda fuera de esta fase (aditivo, sin cambios).
    let columns = simulate_gbm_columns_at::<B>(device, &simulation_times, s0, r, q, sigma, n_paths, seed, 0.0);

    // Un vector de exposiciones (una por ruta) por cada `t` de `exposure_times` -- se agregan al
    // final, ya con todas las rutas evaluadas (la mediana/percentil 95 necesita el vector
    // completo, a diferencia de la media).
    let mut exposures_by_time: Vec<Vec<f64>> = vec![Vec::with_capacity(n_paths_usize); exposure_times.len()];
    for path_idx in 0..n_paths_usize {
        let values: Vec<f64> = columns.iter().map(|col| col[path_idx]).collect();
        let path = SinglePath { times: &simulation_times, values: &values, sigma };
        let (ledger, _events) =
            evaluate_with_events_seeded(&payoff, &path, bridge_seed_for_path(seed, path_idx));
        for (k, &t) in exposure_times.iter().enumerate() {
            let remaining_value: f64 = ledger
                .iter()
                .filter(|cf| cf.payment_time >= t - 1e-9)
                .map(|cf| cf.amount * (-r * (cf.payment_time - t)).exp())
                .sum();
            exposures_by_time[k].push(remaining_value.max(0.0));
        }
    }

    let mut ee = Vec::with_capacity(exposure_times.len());
    let mut pfe_95 = Vec::with_capacity(exposure_times.len());
    for exposures in &mut exposures_by_time {
        let mean = exposures.iter().sum::<f64>() / exposures.len() as f64;
        exposures.sort_by(|a, b| a.partial_cmp(b).expect("payoff: exposicion no finita"));
        let q_idx =
            ((0.95 * exposures.len() as f64).ceil() as usize).saturating_sub(1).min(exposures.len() - 1);
        ee.push(mean);
        pfe_95.push(exposures[q_idx]);
    }

    Ok(ExposureProfile { times: exposure_times.to_vec(), ee, pfe_95 })
}

/// `true` si `spec_json` compila y contiene al menos un `ContractOp::Exercise` (PLAN_GREEKS.md
/// §11 Fase 7): consulta de capacidad usada por `engine::greeks::compute_greek` (C++) para decidir
/// ANTES de llamar a `payoff_sensitivity_gbm_q` si esa llamada va a resolverse via pathwise o via
/// el fallback bump-and-reval interno de esa funcion -- sin esto, `GreekResult::method_used`
/// reportaria "Pathwise" incluso cuando Rust decidio bump-and-reval por su cuenta (§5.1: "un
/// contrato con ContractOp::Exercise sigue cayendo al fallback"), violando la garantia de
/// trazabilidad de §13 ("ningun resultado de Greek se sirve sin saber de donde salio"). Mismo
/// preflight de compilacion que el resto de funciones publicas de este modulo (`Err` si
/// `spec_json` no compila), nunca oculta un JSON invalido detras de un `false`.
pub fn payoff_contains_exercise(spec_json: &str) -> Result<bool, String> {
    let payoff = compile(spec_json)?;
    Ok(contains_exercise(&payoff))
}

/// `true` si `spec_json` compila y depende del subyacente en una UNICA fecha terminal (sin
/// dependencia de trayectoria) -- consulta de capacidad usada por `engine::greeks::compute_greek`
/// (C++) para decidir, ANTES de llamar a `payoff_sensitivity2_gbm_q`/`payoff_sensitivity_cross_gbm_q`,
/// si Gamma/Vanna via likelihood ratio aplican a este contrato (PLAN_HYPERDUAL.md §5) o si debe
/// caer a su estencil generico de bump-and-reval. Mismo criterio de preflight que
/// `payoff_contains_exercise` (`Err` si `spec_json` no compila, nunca oculta un JSON invalido
/// detras de un `false`).
pub fn payoff_supports_second_order_lrm(spec_json: &str) -> Result<bool, String> {
    let payoff = compile(spec_json)?;
    Ok(lrm::single_terminal_time(&payoff).is_ok())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::models::gbm::black_scholes_call;

    const CALL_JSON_TEMPLATE: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "TEST_CALL",
        "contract": {
            "type": "when",
            "time": {maturity},
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "max",
                    "left": {
                        "type": "sub",
                        "left": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}},
                        "right": {"type": "constant", "value": {strike}}
                    },
                    "right": {"type": "constant", "value": 0.0}
                }
            }
        }
    }"#;

    #[test]
    fn preflight_rejects_an_observable_the_model_does_not_generate() {
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        let err = price_payoff_gbm_q("cpu", &spec, "EQ.SPOT.OTHER_TICKER", 100.0, 0.05, 0.0, 0.2, 1_000, 7, 0.0)
            .expect_err("un observable distinto del generado por el modelo debe fallar en preflight");
        assert!(err.contains("EQ.SPOT.XYZ"));
        assert!(err.contains("no generado"));
    }

    #[test]
    fn monte_carlo_price_of_a_payoff_call_converges_to_black_scholes() {
        // PLAN.md §7.19: lock de RNG global, ver crate::rng_test_lock.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE
            .replace("{maturity}", &maturity.to_string())
            .replace("{strike}", &strike.to_string());

        let estimate = price_payoff_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, 200_000, 7, 0.0).unwrap();
        let analytic = black_scholes_call(s0, strike, r, q, sigma, maturity);

        let tolerance = 8.0 * estimate.std_error;
        assert!(
            (estimate.mean - analytic).abs() < tolerance,
            "MC={} analytic={analytic} tol={tolerance} (std_error={})",
            estimate.mean,
            estimate.std_error
        );
    }

    #[test]
    fn q_price_has_no_physical_drift_parameter_to_change() {
        // Fase 5, criterio de aceptacion: "cambiar el drift fisico no afecta un precio Q". Esta
        // funcion no expone NINGUN parametro de drift fisico -- solo `r` (libre de riesgo) y `q`
        // (dividend yield), ambos parte de la propia definicion de la medida Q (ver el
        // doc-comment del modulo y de `models::gbm::Gbm`) -- asi que no hay ningun "drift fisico"
        // que pueda cambiarse sin cambiar tambien `r`/`q`, en cuyo caso ya no seria el mismo
        // precio Q por definicion, no una fuga de un parametro P. Este test fija esa garantia
        // estructural en la firma (`price_payoff_gbm_q` toma exactamente `s0/r/q/sigma`, ninguno
        // documentado como "fisico"): dos llamadas con distinta seed sobre el MISMO `(r, q,
        // sigma)` deben seguir siendo estimaciones del mismo precio Q, dentro de sus propios
        // intervalos de confianza -- no hay margen para que un dato fisico externo se cuele.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        let (s0, r, q, sigma) = (100.0, 0.05, 0.0, 0.2);
        let first = price_payoff_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, 50_000, 7, 0.0).unwrap();
        let second = price_payoff_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, 50_000, 99, 0.0).unwrap();
        let combined_tolerance = 8.0 * (first.std_error + second.std_error);
        assert!(
            (first.mean - second.mean).abs() < combined_tolerance,
            "first={first:?} second={second:?} tol={combined_tolerance}"
        );
    }

    // PLAN_PRODUCTS.md §12 Fase 6 ("barrera discreta vectorizada bajo Q"): un up-and-in call
    // (barrera H, strike K, ambos monitorizados en `monitoring_times`) que paga en `maturity` si
    // el spot toca H en algun instante monitorizado, o cero si nunca lo toca.
    fn up_and_in_call_json(barrier: f64, strike: f64, maturity: f64, monitoring_times: &[f64]) -> String {
        let times_json = monitoring_times.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(",");
        format!(
            r#"{{
                "schema": "engine.payoff/v1", "id": "UI",
                "contract": {{
                    "type": "trigger", "id": "UI",
                    "monitoring_times": [{times_json}],
                    "condition": {{"type": "greater_equal",
                        "left": {{"type": "current", "observable": "EQ.SPOT.XYZ"}},
                        "right": {{"type": "constant", "value": {barrier}}}}},
                    "monitoring": "discrete", "settlement": "at_scheduled_payment", "priority": 0, "latch": true,
                    "on_hit": {{"type": "when", "time": {maturity},
                        "child": {{"type": "cashflow", "currency": "USD",
                            "amount": {{"type": "max",
                                "left": {{"type": "sub",
                                    "left": {{"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}},
                                    "right": {{"type": "constant", "value": {strike}}}}},
                                "right": {{"type": "constant", "value": 0.0}}}}}}}},
                    "on_miss": {{"type": "zero"}}
                }}
            }}"#
        )
    }

    // Complemento: up-and-out (paga el mismo intrinseco solo si la barrera NUNCA se toca; cero si
    // se toca -- "on_hit"/"on_miss" intercambiados frente a `up_and_in_call_json`, mismo barrier/
    // strike/monitoring_times). §13.2: "un knock-in + knock-out complementarios reproducen el
    // underlying" -- UI + UO debe reproducir exactamente la call vanilla en CADA ruta.
    fn up_and_out_call_json(barrier: f64, strike: f64, maturity: f64, monitoring_times: &[f64]) -> String {
        let times_json = monitoring_times.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(",");
        format!(
            r#"{{
                "schema": "engine.payoff/v1", "id": "UO",
                "contract": {{
                    "type": "trigger", "id": "UO",
                    "monitoring_times": [{times_json}],
                    "condition": {{"type": "greater_equal",
                        "left": {{"type": "current", "observable": "EQ.SPOT.XYZ"}},
                        "right": {{"type": "constant", "value": {barrier}}}}},
                    "monitoring": "discrete", "settlement": "at_scheduled_payment", "priority": 0, "latch": true,
                    "on_hit": {{"type": "zero"}},
                    "on_miss": {{"type": "when", "time": {maturity},
                        "child": {{"type": "cashflow", "currency": "USD",
                            "amount": {{"type": "max",
                                "left": {{"type": "sub",
                                    "left": {{"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}},
                                    "right": {{"type": "constant", "value": {strike}}}}},
                                "right": {{"type": "constant", "value": 0.0}}}}}}}}
                }}
            }}"#
        )
    }

    #[test]
    fn up_and_in_plus_up_and_out_reproduces_the_vanilla_call_price() {
        // §13.2 ("un knock-in + knock-out complementarios reproducen el underlying"): en CADA
        // ruta simulada para UI/UO (mismo grid de monitorizacion de 4 puntos), o bien UI paga el
        // intrinseco y UO paga cero, o al reves -- la suma de los ledgers coincide EXACTAMENTE
        // con el intrinseco de la call vanilla en esa misma ruta. `vanilla` se precia aqui con su
        // propio grid natural (un unico paso, `required_times()={maturity}`): la comparacion
        // frente a `ui.mean + uo.mean` es entonces estadistica (ambas simulaciones son GBM exacto
        // -- sin sesgo de discretizacion -- asi que coinciden en esperanza, no path-a-path, al
        // usar un numero de pasos distinto para cada una).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let monitoring_times = [0.25, 0.5, 0.75, 1.0];
        let (n_paths, seed) = (300_000, 7);

        let ui_spec = up_and_in_call_json(barrier, strike, maturity, &monitoring_times);
        let uo_spec = up_and_out_call_json(barrier, strike, maturity, &monitoring_times);
        let vanilla_spec = CALL_JSON_TEMPLATE
            .replace("{maturity}", &maturity.to_string())
            .replace("{strike}", &strike.to_string());

        let ui = price_payoff_gbm_q("cpu", &ui_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
        let uo = price_payoff_gbm_q("cpu", &uo_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed + 1, 0.0).unwrap();
        let vanilla = price_payoff_gbm_q("cpu", &vanilla_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed + 2, 0.0).unwrap();

        let combined_std_error = (ui.std_error.powi(2) + uo.std_error.powi(2) + vanilla.std_error.powi(2)).sqrt();
        let tolerance = 8.0 * combined_std_error;
        assert!(
            (ui.mean + uo.mean - vanilla.mean).abs() < tolerance,
            "ui={} uo={} ui+uo={} vanilla={} tol={tolerance}",
            ui.mean,
            uo.mean,
            ui.mean + uo.mean,
            vanilla.mean
        );
    }

    #[test]
    fn up_and_in_price_increases_as_the_monitoring_grid_is_refined() {
        // Mas puntos de monitorizacion == mas oportunidades de tocar la barrera == probabilidad
        // de activacion no decreciente == precio up-and-in no decreciente (converge hacia el
        // limite de monitorizacion continua al refinar la malla, PLAN_PRODUCTS.md §12 Fase 6,
        // criterio de aceptacion "convergencia de barreras al refinar malla").
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (300_000, 11);

        let coarse_times: Vec<f64> = vec![0.25, 0.5, 0.75, 1.0];
        let fine_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();

        let coarse_spec = up_and_in_call_json(barrier, strike, maturity, &coarse_times);
        let fine_spec = up_and_in_call_json(barrier, strike, maturity, &fine_times);

        let coarse = price_payoff_gbm_q("cpu", &coarse_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
        let fine = price_payoff_gbm_q("cpu", &fine_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();

        let tolerance = 8.0 * (coarse.std_error + fine.std_error);
        assert!(
            fine.mean + tolerance >= coarse.mean,
            "fine={} (se={}) deberia ser >= coarse={} (se={}) menos ruido estadistico",
            fine.mean,
            fine.std_error,
            coarse.mean,
            coarse.std_error
        );
    }

    // PLAN_PRODUCTS.md §12 Fase 6 ("hit probability Q como medida separada del PV").
    #[test]
    fn hit_probability_preflight_rejects_an_event_that_does_not_exist() {
        let spec = up_and_in_call_json(120.0, 100.0, 1.0, &[0.25, 0.5, 0.75, 1.0]);
        let err = hit_probability_gbm_q("cpu", &spec, "NOT_A_REAL_EVENT", "EQ.SPOT.XYZ", 100.0, 0.05, 0.0, 0.2, 1_000, 7)
            .expect_err("un evento que no existe en el contrato debe fallar en preflight");
        assert!(err.contains("NOT_A_REAL_EVENT"));
    }

    // PLAN_PRODUCTS.md §12 Fase 6 ("Brownian bridge para modelos compatibles"): la
    // aproximacion continua solo puede AÑADIR probabilidad de hit sobre la misma malla discreta
    // (nunca quitarla), y esa diferencia debe encogerse al refinar la malla (el bridge se vuelve
    // menos relevante cuantos mas puntos discretos ya cubren la trayectoria).
    #[test]
    fn continuous_approximation_price_is_at_least_the_discrete_price_on_the_same_coarse_grid() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let coarse_times = [0.25, 0.5, 0.75, 1.0];
        let (n_paths, seed) = (300_000, 13);

        let discrete_spec = up_and_in_call_json(barrier, strike, maturity, &coarse_times);
        let continuous_spec = discrete_spec.replace("\"discrete\"", "\"continuous_approximation\"");

        let discrete = price_payoff_gbm_q("cpu", &discrete_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
        let continuous =
            price_payoff_gbm_q("cpu", &continuous_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();

        let tolerance = 8.0 * (discrete.std_error + continuous.std_error);
        assert!(
            continuous.mean + tolerance >= discrete.mean,
            "continuous={} (se={}) deberia ser >= discrete={} (se={}) menos ruido estadistico",
            continuous.mean,
            continuous.std_error,
            discrete.mean,
            discrete.std_error
        );
    }

    #[test]
    fn continuous_and_discrete_prices_converge_as_the_monitoring_grid_is_refined() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (300_000, 17);

        let coarse_times: Vec<f64> = vec![0.25, 0.5, 0.75, 1.0];
        let fine_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();

        let coarse_discrete = up_and_in_call_json(barrier, strike, maturity, &coarse_times);
        let coarse_continuous = coarse_discrete.replace("\"discrete\"", "\"continuous_approximation\"");
        let fine_discrete = up_and_in_call_json(barrier, strike, maturity, &fine_times);
        let fine_continuous = fine_discrete.replace("\"discrete\"", "\"continuous_approximation\"");

        let coarse_gap = {
            let d = price_payoff_gbm_q("cpu", &coarse_discrete, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
            let c = price_payoff_gbm_q("cpu", &coarse_continuous, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
            (c.mean - d.mean, c.std_error + d.std_error)
        };
        let fine_gap = {
            let d = price_payoff_gbm_q("cpu", &fine_discrete, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
            let c = price_payoff_gbm_q("cpu", &fine_continuous, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
            (c.mean - d.mean, c.std_error + d.std_error)
        };

        // El hueco continuo-menos-discreto en la malla fina debe ser claramente mas pequeno que
        // en la malla gruesa (no solo "no mayor"): con 50 puntos casi no queda hueco que un
        // intervalo pueda esconder.
        assert!(coarse_gap.0 >= 0.0 - 8.0 * coarse_gap.1, "coarse_gap={coarse_gap:?}");
        assert!(fine_gap.0 >= 0.0 - 8.0 * fine_gap.1, "fine_gap={fine_gap:?}");
        assert!(
            fine_gap.0 < coarse_gap.0,
            "fine_gap={} deberia ser menor que coarse_gap={}",
            fine_gap.0,
            coarse_gap.0
        );
    }

    #[test]
    fn hit_probability_is_between_zero_and_one_and_decreases_as_the_barrier_moves_away() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let monitoring_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();
        let (n_paths, seed) = (200_000, 7);

        // Barrera mas cercana al spot (105, 5% OTM) se toca con mas probabilidad que una mas
        // lejana (140, 40% OTM) -- sanity check direccional, no una cota analitica.
        let near_spec = up_and_in_call_json(105.0, strike, maturity, &monitoring_times);
        let far_spec = up_and_in_call_json(140.0, strike, maturity, &monitoring_times);

        let near = hit_probability_gbm_q("cpu", &near_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let far = hit_probability_gbm_q("cpu", &far_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        assert!(near.mean > 0.0 && near.mean < 1.0, "near={}", near.mean);
        assert!(far.mean > 0.0 && far.mean < 1.0, "far={}", far.mean);
        let tolerance = 8.0 * (near.std_error + far.std_error);
        assert!(
            near.mean > far.mean + tolerance,
            "near={} (se={}) deberia ser claramente mayor que far={} (se={})",
            near.mean,
            near.std_error,
            far.mean,
            far.std_error
        );
    }

    #[test]
    fn hit_probability_increases_as_the_monitoring_grid_is_refined() {
        // Mismo argumento de monotonia que `up_and_in_price_increases_as_the_monitoring_grid_is_refined`,
        // pero sobre la probabilidad de hit en si misma en vez del precio derivado de ella.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (300_000, 11);

        let coarse_times: Vec<f64> = vec![0.25, 0.5, 0.75, 1.0];
        let fine_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();

        let coarse_spec = up_and_in_call_json(barrier, strike, maturity, &coarse_times);
        let fine_spec = up_and_in_call_json(barrier, strike, maturity, &fine_times);

        let coarse = hit_probability_gbm_q("cpu", &coarse_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let fine = hit_probability_gbm_q("cpu", &fine_spec, "UI", "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        let tolerance = 8.0 * (coarse.std_error + fine.std_error);
        assert!(
            fine.mean + tolerance >= coarse.mean,
            "fine={} (se={}) deberia ser >= coarse={} (se={}) menos ruido estadistico",
            fine.mean,
            fine.std_error,
            coarse.mean,
            coarse.std_error
        );
    }

    // PLAN_PRODUCTS.md §12 Fase 6 ("perfil de exposicion pathwise a partir del mismo AST").
    #[test]
    fn exposure_at_maturity_and_at_zero_match_the_pv_within_statistical_tolerance_for_a_nonnegative_payoff() {
        // Con la MISMA seed/n_paths/tiempos requeridos (una call europea solo depende de
        // 'maturity'), price_payoff_gbm_q y payoff_exposure_profile_gbm_q simulan la MISMA
        // distribucion de rutas (no necesariamente identicas path a path bajo ejecucion paralela
        // de tests -- el RNG global de Burn no garantiza reproducibilidad bit a bit entre dos
        // llamadas bajo `cargo test` en paralelo, solo dentro de una misma llamada). Como el
        // payoff de una call ya es >= 0, max(V_t,0)=V_t para t=0 y t=maturity -- EE(0) y
        // EE(maturity) deben coincidir con el PV dentro de un margen estadistico generoso.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE
            .replace("{maturity}", &maturity.to_string())
            .replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (200_000, 7);

        let price = price_payoff_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
        let profile = payoff_exposure_profile_gbm_q(
            "cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, &[0.0, maturity], n_paths, seed,
        )
        .unwrap();

        // EE(t) esta expresado EN EL INSTANTE t, no descontado a hoy (§14: "el visitor de
        // valoracion decide como descontar", y una exposicion futura se reporta a esa fecha, no
        // traida a valor presente): EE(0) coincide con el PV (nada que descontar entre 0 y 0),
        // pero EE(maturity) es el payoff SIN descontar desde maturity -- price.mean*exp(r*maturity).
        assert_eq!(profile.times, vec![0.0, maturity]);
        let tolerance_at_zero = 8.0 * price.std_error;
        assert!(
            (profile.ee[0] - price.mean).abs() < tolerance_at_zero,
            "ee(0)={} price={} tol={tolerance_at_zero}",
            profile.ee[0],
            price.mean
        );
        let undiscounted_payoff_mean = price.mean * (r * maturity).exp();
        let tolerance_at_maturity = 8.0 * price.std_error * (r * maturity).exp();
        assert!(
            (profile.ee[1] - undiscounted_payoff_mean).abs() < tolerance_at_maturity,
            "ee(maturity)={} payoff_mean_sin_descontar={} tol={tolerance_at_maturity}",
            profile.ee[1],
            undiscounted_payoff_mean
        );
    }

    #[test]
    fn exposure_profile_nets_a_long_and_a_short_of_the_same_trade_to_zero() {
        // Portfolio = Both(call larga, Give(la misma call)): el ledger neteado es Zero en TODAS
        // las rutas -- confirma que el netting es automatico por composicion del AST (§11), sin
        // ningun paso de netting aparte en la medida de exposicion.
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let call_leg = format!(
            r#"{{"type": "when", "time": {maturity}, "child": {{"type": "cashflow", "currency": "USD",
                "amount": {{"type": "max",
                    "left": {{"type": "sub",
                        "left": {{"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}},
                        "right": {{"type": "constant", "value": {strike}}}}},
                    "right": {{"type": "constant", "value": 0.0}}}}}}}}"#
        );
        let spec = format!(
            r#"{{"schema": "engine.payoff/v1", "id": "NET", "contract": {{"type": "both", "children": [
                {call_leg}, {{"type": "give", "child": {call_leg}}}
            ]}}}}"#
        );

        let profile = payoff_exposure_profile_gbm_q(
            "cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, &[0.5, maturity], 20_000, 7,
        )
        .unwrap();

        assert_eq!(profile.ee, vec![0.0, 0.0]);
        assert_eq!(profile.pfe_95, vec![0.0, 0.0]);
    }

    #[test]
    fn exposure_profile_rejects_empty_or_negative_exposure_times() {
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        assert!(payoff_exposure_profile_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", 100.0, 0.05, 0.0, 0.2, &[], 1_000, 7).is_err());
        assert!(
            payoff_exposure_profile_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", 100.0, 0.05, 0.0, 0.2, &[-1.0], 1_000, 7).is_err()
        );
    }

    #[test]
    fn exposure_profile_pfe95_dominates_ee_for_a_barrier_payoff() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let monitoring_times = [0.25, 0.5, 0.75, 1.0];
        let spec = up_and_in_call_json(barrier, strike, maturity, &monitoring_times);

        let profile = payoff_exposure_profile_gbm_q(
            "cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, &[0.5, 0.75, maturity], 100_000, 7,
        )
        .unwrap();

        for (ee, pfe) in profile.ee.iter().zip(profile.pfe_95.iter()) {
            assert!(*ee >= 0.0, "ee={ee}");
            assert!(*pfe >= *ee, "pfe={pfe} deberia dominar a ee={ee}");
        }
    }

    // Exercise / Longstaff-Schwartz (PLAN_PRODUCTS.md §10, Fase 9). `dates` son fechas de
    // ejercicio anticipado ESTRICTAMENTE anteriores a `maturity` -- `continuation` ya paga el
    // intrinseco europeo en `maturity`, asi que incluir `maturity` en `dates` seria redundante
    // (la decision ahi coincidiria siempre con la propia continuacion).
    // Correccion de un bug preexistente encontrado durante Fase 11 (no introducido por este
    // cambio): `ev_left`/`ev_right`/`cont_left`/`cont_right` se combinaban antes con
    // `{"type": "max", "left": ..., "right": ...}` DIRECTAMENTE sobre strike/spot, produciendo
    // `max(K, S)` en vez de `max(K - S, 0)` (payoff de un put) -- una cantidad SIEMPRE >= K y
    // creciente en S, el reverso exacto de un put real. Las comparaciones relativas de las
    // pruebas de esta seccion (americana >= europea, monotonia en fechas, americana==europea sin
    // dividendos para una call) seguian pasando por accidente (se cumplen para ambas formas), asi
    // que el bug nunca se manifesto hasta pedir el SIGNO de la sensibilidad respecto del spot
    // (`sensitivity_of_a_bermuda_put_...`, mas abajo), que si lo expuso. Ahora ambas ramas restan
    // explicitamente antes del `max(..., 0)`, igual que `CALL_JSON_TEMPLATE`/`european_vanilla_json`.
    fn bermuda_contract_json(event_id: &str, kind: &str, strike: f64, maturity: f64, dates: &[f64]) -> String {
        let dates_json = dates.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(",");
        let (ev_left, ev_right) = match kind {
            "put" => (
                r#"{"type": "sub", "left": {"type": "constant", "value": {strike}},
                    "right": {"type": "current", "observable": "EQ.SPOT.XYZ"}}"#,
                r#"{"type": "constant", "value": 0.0}"#,
            ),
            "call" => (
                r#"{"type": "sub", "left": {"type": "current", "observable": "EQ.SPOT.XYZ"},
                    "right": {"type": "constant", "value": {strike}}}"#,
                r#"{"type": "constant", "value": 0.0}"#,
            ),
            other => panic!("kind desconocido: {other}"),
        };
        let (cont_left, cont_right) = match kind {
            "put" => (
                r#"{"type": "sub", "left": {"type": "constant", "value": {strike}},
                    "right": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}}"#,
                r#"{"type": "constant", "value": 0.0}"#,
            ),
            "call" => (
                r#"{"type": "sub", "left": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}},
                    "right": {"type": "constant", "value": {strike}}}"#,
                r#"{"type": "constant", "value": 0.0}"#,
            ),
            other => panic!("kind desconocido: {other}"),
        };
        format!(
            r#"{{
                "type": "exercise", "id": "{event_id}",
                "dates": [{dates_json}],
                "exercise_value": {{"type": "max", "left": {ev_left}, "right": {ev_right}}},
                "continuation": {{"type": "when", "time": {maturity}, "child": {{"type": "cashflow",
                    "currency": "USD",
                    "amount": {{"type": "max", "left": {cont_left}, "right": {cont_right}}}}}}}
            }}"#
        )
        .replace("{strike}", &strike.to_string())
        .replace("{maturity}", &maturity.to_string())
    }

    fn bermuda_json(kind: &str, strike: f64, maturity: f64, dates: &[f64]) -> String {
        let contract = bermuda_contract_json("EX", kind, strike, maturity, dates);
        format!(r#"{{"schema": "engine.payoff/v1", "id": "BERMUDA_{kind}", "contract": {contract}}}"#)
    }

    // Mismo bug preexistente que `bermuda_contract_json` (ver su comentario): corregido a
    // `max(K - S, 0)` / `max(S - K, 0)` en vez de `max(K, S)` / `max(S, K)`.
    fn european_vanilla_json(kind: &str, strike: f64, maturity: f64) -> String {
        let (left, right) = match kind {
            "put" => (
                r#"{"type": "sub", "left": {"type": "constant", "value": {strike}},
                    "right": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}}}"#,
                r#"{"type": "constant", "value": 0.0}"#,
            ),
            "call" => (
                r#"{"type": "sub", "left": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": {maturity}},
                    "right": {"type": "constant", "value": {strike}}}"#,
                r#"{"type": "constant", "value": 0.0}"#,
            ),
            other => panic!("kind desconocido: {other}"),
        };
        format!(
            r#"{{"schema": "engine.payoff/v1", "id": "EUROPEAN_{kind}", "contract": {{"type": "when",
                "time": {maturity}, "child": {{"type": "cashflow", "currency": "USD",
                "amount": {{"type": "max", "left": {left}, "right": {right}}}}}}}}}"#
        )
        .replace("{strike}", &strike.to_string())
        .replace("{maturity}", &maturity.to_string())
    }

    #[test]
    fn exercise_preflight_rejects_a_contract_without_any_exercise_node() {
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        let err = price_payoff_exercise_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", 100.0, 0.05, 0.0, 0.2, 1_000, 7)
            .expect_err("un contrato sin Exercise debe fallar en preflight");
        assert!(err.contains("Exercise"));
    }

    #[test]
    fn exercise_preflight_rejects_more_than_one_exercise_node() {
        let leg1 = bermuda_contract_json("EX1", "put", 100.0, 1.0, &[0.5]);
        let leg2 = bermuda_contract_json("EX2", "put", 100.0, 1.0, &[0.5]);
        let spec = format!(
            r#"{{"schema": "engine.payoff/v1", "id": "TWO_EXERCISE", "contract": {{"type": "both",
                "children": [{leg1}, {leg2}]}}}}"#
        );
        let err = price_payoff_exercise_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", 100.0, 0.05, 0.0, 0.2, 1_000, 7)
            .expect_err("dos nodos Exercise en el mismo contrato deben rechazarse (alcance de Fase 9)");
        assert!(err.contains("Exercise"));
    }

    #[test]
    fn bermuda_put_price_is_at_least_the_european_put_price_without_dividends() {
        // PLAN_PRODUCTS.md §12 Fase 9, criterio de aceptacion explicito: "americana >= europea
        // para put sin dividendos" -- el derecho de ejercicio anticipado nunca puede valer menos
        // que no tenerlo.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (200_000, 21);

        let european_spec = european_vanilla_json("put", strike, maturity);
        let bermuda_spec = bermuda_json("put", strike, maturity, &[0.25, 0.5, 0.75]);

        let european = price_payoff_gbm_q("cpu", &european_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
        let bermuda =
            price_payoff_exercise_gbm_q("cpu", &bermuda_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed + 1).unwrap();

        let tolerance = 8.0 * (european.std_error + bermuda.price.std_error);
        assert!(
            bermuda.price.mean + tolerance >= european.mean,
            "bermuda={} (se={}) deberia ser >= europea={} (se={})",
            bermuda.price.mean,
            bermuda.price.std_error,
            european.mean,
            european.std_error
        );
    }

    #[test]
    fn bermuda_put_price_increases_as_more_exercise_dates_are_added() {
        // "convergencia por fechas": mas oportunidades de ejercicio anticipado nunca reducen el
        // valor de la opcion (mismo argumento de monotonia que las barreras de Fase 6, aqui sobre
        // el numero de fechas de decision en vez del numero de puntos de monitorizacion).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (200_000, 23);

        let sparse_spec = bermuda_json("put", strike, maturity, &[0.5]);
        let dense_spec = bermuda_json("put", strike, maturity, &[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9]);

        let sparse =
            price_payoff_exercise_gbm_q("cpu", &sparse_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let dense = price_payoff_exercise_gbm_q("cpu", &dense_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        let tolerance = 8.0 * (sparse.price.std_error + dense.price.std_error);
        assert!(
            dense.price.mean + tolerance >= sparse.price.mean,
            "dense={} (se={}) deberia ser >= sparse={} (se={})",
            dense.price.mean,
            dense.price.std_error,
            sparse.price.mean,
            sparse.price.std_error
        );
    }

    #[test]
    fn bermuda_call_price_matches_the_european_call_price_without_dividends() {
        // Resultado clasico (sin dividendos, sin costes de carry negativos): nunca es optimo
        // ejercer una call americana anticipadamente -- la politica de Longstaff-Schwartz deberia
        // descubrirlo por si misma y el precio bermuda/americano debe coincidir con el europeo
        // dentro de tolerancia estadistica (no solo >=).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let (n_paths, seed) = (200_000, 29);

        let european_spec = european_vanilla_json("call", strike, maturity);
        let bermuda_spec = bermuda_json("call", strike, maturity, &[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9]);

        let european = price_payoff_gbm_q("cpu", &european_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
        let bermuda =
            price_payoff_exercise_gbm_q("cpu", &bermuda_spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed + 1).unwrap();

        let tolerance = 8.0 * (european.std_error + bermuda.price.std_error);
        assert!(
            (bermuda.price.mean - european.mean).abs() < tolerance,
            "bermuda={} (se={}) europea={} (se={}) tol={tolerance}",
            bermuda.price.mean,
            bermuda.price.std_error,
            european.mean,
            european.std_error
        );
    }

    #[test]
    fn exercise_decisions_are_reproducible_given_the_same_seed() {
        // "decisiones reproducibles con seed": misma seed, mismo precio Y mismo diagnostico de
        // politica -- ninguna de las dos pasadas (simulacion, regresion) consume aleatoriedad
        // fuera de la sembrada explicitamente. El guard es necesario aqui por la MISMA razon que
        // en el resto de este archivo (ver el doc-comment de rng_test_lock): CpuBackend::seed
        // fija un estado GLOBAL de Burn, asi que dos llamadas que dependen de la MISMA seed deben
        // quedar serializadas frente a cualquier otro test de este binario que tambien siembre el
        // backend en paralelo -- sin el guard, esta prueba compara dos ejecuciones que en
        // realidad NO compartieron el mismo estado de RNG (la propia causa de la "no
        // reproducibilidad" que fallaria aqui, no un bug del pricer).
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = bermuda_json("put", strike, maturity, &[0.25, 0.5, 0.75]);
        let (n_paths, seed) = (20_000, 41);

        let first = price_payoff_exercise_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let second = price_payoff_exercise_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        assert_eq!(first.price.mean, second.price.mean);
        assert_eq!(first.price.std_error, second.price.std_error);
        assert_eq!(first.dates.len(), second.dates.len());
        for (a, b) in first.dates.iter().zip(second.dates.iter()) {
            assert_eq!(a.date, b.date);
            assert_eq!(a.n_in_the_money, b.n_in_the_money);
            assert_eq!(a.exercised_fraction, b.exercised_fraction);
        }
    }

    #[test]
    fn exercise_policy_diagnostics_are_reported_in_ascending_date_order_with_plausible_fields() {
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = bermuda_json("put", strike, maturity, &[0.25, 0.5, 0.75]);

        let result = price_payoff_exercise_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, 20_000, 7).unwrap();

        assert_eq!(result.dates.len(), 3);
        assert_eq!(result.dates.iter().map(|d| d.date).collect::<Vec<_>>(), vec![0.25, 0.5, 0.75]);
        for d in &result.dates {
            assert!(d.exercised_fraction >= 0.0 && d.exercised_fraction <= 1.0, "frac={}", d.exercised_fraction);
            if d.regression_coeffs.is_none() {
                assert_eq!(d.exercised_fraction, 0.0);
            }
        }
    }

    // Sensibilidades pathwise (PLAN_PRODUCTS.md §12 Fase 11: "AAD con fallback a bump-and-reval
    // para sensibilidades del pricer Monte Carlo GBM de payoff"). El oraculo es una diferencia
    // central de la formula CERRADA de Black-Scholes (`black_scholes_call`, deterministica, sin
    // ruido de Monte Carlo) -- la unica fuente de tolerancia es el propio estimador Monte Carlo
    // bajo prueba (`delta.std_error`), no el oraculo.
    #[test]
    fn sensitivity_delta_of_a_call_matches_finite_difference_of_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", &maturity.to_string()).replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (200_000, 7);

        let delta =
            payoff_sensitivity_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "spot", s0, r, q, sigma, n_paths, seed).unwrap();

        let h = 1e-3;
        let analytic_delta = (black_scholes_call(s0 + h, strike, r, q, sigma, maturity)
            - black_scholes_call(s0 - h, strike, r, q, sigma, maturity))
            / (2.0 * h);

        let tolerance = 8.0 * delta.std_error;
        assert!(
            (delta.mean - analytic_delta).abs() < tolerance,
            "delta MC={} (se={}) delta analitica={analytic_delta} tol={tolerance}",
            delta.mean,
            delta.std_error
        );
    }

    #[test]
    fn sensitivity_vega_of_a_call_matches_finite_difference_of_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", &maturity.to_string()).replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (200_000, 11);

        let vega =
            payoff_sensitivity_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "volatility", s0, r, q, sigma, n_paths, seed)
                .unwrap();

        let h = 1e-3;
        let analytic_vega = (black_scholes_call(s0, strike, r, q, sigma + h, maturity)
            - black_scholes_call(s0, strike, r, q, sigma - h, maturity))
            / (2.0 * h);

        let tolerance = 8.0 * vega.std_error;
        assert!(
            (vega.mean - analytic_vega).abs() < tolerance,
            "vega MC={} (se={}) vega analitica={analytic_vega} tol={tolerance}",
            vega.mean,
            vega.std_error
        );
    }

    #[test]
    fn sensitivity_rho_of_a_call_matches_finite_difference_of_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", &maturity.to_string()).replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (200_000, 13);

        let rho = payoff_sensitivity_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "rate", s0, r, q, sigma, n_paths, seed).unwrap();

        let h = 1e-3;
        let analytic_rho = (black_scholes_call(s0, strike, r + h, q, sigma, maturity)
            - black_scholes_call(s0, strike, r - h, q, sigma, maturity))
            / (2.0 * h);

        let tolerance = 8.0 * rho.std_error;
        assert!(
            (rho.mean - analytic_rho).abs() < tolerance,
            "rho MC={} (se={}) rho analitica={analytic_rho} tol={tolerance}",
            rho.mean,
            rho.std_error
        );
    }

    #[test]
    fn sensitivity_of_dividend_yield_of_a_call_matches_finite_difference_of_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.02, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", &maturity.to_string()).replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (200_000, 17);

        let sensitivity =
            payoff_sensitivity_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "dividend_yield", s0, r, q, sigma, n_paths, seed)
                .unwrap();

        let h = 1e-3;
        let analytic = (black_scholes_call(s0, strike, r, q + h, sigma, maturity)
            - black_scholes_call(s0, strike, r, q - h, sigma, maturity))
            / (2.0 * h);

        let tolerance = 8.0 * sensitivity.std_error;
        assert!(
            (sensitivity.mean - analytic).abs() < tolerance,
            "MC={} (se={}) analitica={analytic} tol={tolerance}",
            sensitivity.mean,
            sensitivity.std_error
        );
    }

    // PLAN_HYPERDUAL.md §5: Gamma/Vanna via likelihood ratio (payoff::lrm) contra la formula
    // cerrada de Black-Scholes -- mismo oraculo/criterio que los tests de orden 1 de arriba.
    #[test]
    fn sensitivity2_gamma_of_a_call_matches_second_finite_difference_of_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", &maturity.to_string()).replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (500_000, 7);

        let gamma =
            payoff_sensitivity2_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "spot", s0, r, q, sigma, n_paths, seed).unwrap();

        let h = 1e-2;
        let analytic_gamma = (black_scholes_call(s0 + h, strike, r, q, sigma, maturity)
            - 2.0 * black_scholes_call(s0, strike, r, q, sigma, maturity)
            + black_scholes_call(s0 - h, strike, r, q, sigma, maturity))
            / (h * h);

        let tolerance = 8.0 * gamma.std_error;
        assert!(gamma.mean > 0.0, "la Gamma de una call vainilla es siempre positiva");
        assert!(
            (gamma.mean - analytic_gamma).abs() < tolerance,
            "gamma MC={} (se={}) gamma analitica={analytic_gamma} tol={tolerance}",
            gamma.mean,
            gamma.std_error
        );
    }

    #[test]
    fn sensitivity_cross_vanna_of_a_call_matches_mixed_finite_difference_of_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", &maturity.to_string()).replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (500_000, 11);

        let vanna = payoff_sensitivity_cross_gbm_q(
            "cpu", &spec, "EQ.SPOT.XYZ", "spot", "volatility", s0, r, q, sigma, n_paths, seed,
        )
        .unwrap();

        let (h_spot, h_vol) = (1.0, 0.002);
        let analytic_vanna = (black_scholes_call(s0 + h_spot, strike, r, q, sigma + h_vol, maturity)
            - black_scholes_call(s0 + h_spot, strike, r, q, sigma - h_vol, maturity)
            - black_scholes_call(s0 - h_spot, strike, r, q, sigma + h_vol, maturity)
            + black_scholes_call(s0 - h_spot, strike, r, q, sigma - h_vol, maturity))
            / (4.0 * h_spot * h_vol);

        let tolerance = 8.0 * vanna.std_error;
        assert!(
            (vanna.mean - analytic_vanna).abs() < tolerance,
            "vanna MC={} (se={}) vanna analitica={analytic_vanna} tol={tolerance}",
            vanna.mean,
            vanna.std_error
        );
    }

    // PLAN_BACKWARD.md §9 Fase 1: `payoff_local_hessian_gbm_q` hace UNA sola simulacion reutilizada
    // para Gamma/Volga/Vanna -- con el MISMO seed/n_paths que las llamadas separadas de arriba,
    // Gamma/Vanna deben coincidir BIT A BIT (mismas muestras, misma formula, mismo orden de
    // operaciones en punto flotante) y Volga debe coincidir con Black-Scholes dentro de tolerancia
    // Monte Carlo.
    #[test]
    fn local_hessian_matches_separate_gamma_and_vanna_calls_bit_for_bit_and_volga_matches_black_scholes() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", &maturity.to_string()).replace("{strike}", &strike.to_string());
        let (n_paths, seed) = (200_000, 7);

        let hessian = payoff_local_hessian_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();
        let gamma_separate =
            payoff_sensitivity2_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "spot", s0, r, q, sigma, n_paths, seed).unwrap();
        let vanna_separate = payoff_sensitivity_cross_gbm_q(
            "cpu", &spec, "EQ.SPOT.XYZ", "spot", "volatility", s0, r, q, sigma, n_paths, seed,
        )
        .unwrap();

        assert_eq!(
            hessian.gamma, gamma_separate,
            "misma semilla/n_paths/formula -> mismas muestras -> identico bit a bit"
        );
        assert_eq!(
            hessian.vanna, vanna_separate,
            "misma semilla/n_paths/formula -> mismas muestras -> identico bit a bit"
        );

        let h = 1e-3;
        let analytic_volga = (black_scholes_call(s0, strike, r, q, sigma + h, maturity)
            - 2.0 * black_scholes_call(s0, strike, r, q, sigma, maturity)
            + black_scholes_call(s0, strike, r, q, sigma - h, maturity))
            / (h * h);

        let tolerance = 8.0 * hessian.volga.std_error;
        assert!(
            (hessian.volga.mean - analytic_volga).abs() < tolerance,
            "volga MC={} (se={}) volga analitica={analytic_volga} tol={tolerance}",
            hessian.volga.mean,
            hessian.volga.std_error
        );
    }

    #[test]
    fn sensitivity2_and_cross_are_rejected_explicitly_for_a_path_dependent_contract() {
        // PLAN_HYPERDUAL.md §5: Gamma/Vanna via likelihood ratio solo aplican a una unica fecha
        // terminal -- un contrato path-dependiente (aqui, una bermuda con Exercise) se rechaza
        // explicito, sin aproximar en silencio. `compute_greek` (C++) cae a bump-and-reval para
        // este caso.
        let spec = bermuda_json("put", 100.0, 1.0, &[0.25, 0.5, 0.75]);
        let err2 = payoff_sensitivity2_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "spot", 100.0, 0.05, 0.0, 0.2, 1_000, 7)
            .expect_err("un contrato de varias fechas debe rechazarse explicito");
        assert!(err2.contains("fecha"));

        let err_cross = payoff_sensitivity_cross_gbm_q(
            "cpu", &spec, "EQ.SPOT.XYZ", "spot", "volatility", 100.0, 0.05, 0.0, 0.2, 1_000, 7,
        )
        .expect_err("un contrato de varias fechas debe rechazarse explicito");
        assert!(err_cross.contains("fecha"));

        assert!(!payoff_supports_second_order_lrm(&spec).unwrap());
        let vanilla = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        assert!(payoff_supports_second_order_lrm(&vanilla).unwrap());
    }

    #[test]
    fn sensitivity_cross_rejects_a_pair_other_than_spot_and_volatility() {
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        let err = payoff_sensitivity_cross_gbm_q(
            "cpu", &spec, "EQ.SPOT.XYZ", "spot", "rate", 100.0, 0.05, 0.0, 0.2, 1_000, 7,
        )
        .expect_err("solo el par (spot, volatility) esta soportado");
        assert!(err.contains("volatility"));
    }

    #[test]
    fn sensitivity_rejects_an_unknown_greek_name() {
        let spec = CALL_JSON_TEMPLATE.replace("{maturity}", "1.0").replace("{strike}", "100.0");
        let err = payoff_sensitivity_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "theta", 100.0, 0.05, 0.0, 0.2, 1_000, 7)
            .expect_err("un nombre de greek desconocido debe fallar");
        assert!(err.contains("theta"));
    }

    #[test]
    fn sensitivity_of_a_bermuda_put_falls_back_to_bump_and_reval_and_the_spot_delta_is_negative() {
        // El contrato contiene un ContractOp::Exercise -- payoff_sensitivity_gbm_q debe tomar el
        // camino bump-and-reval (ver contains_exercise en el doc-comment del modulo sensitivity),
        // no la pasada pathwise. No hay formula cerrada de referencia para una bermuda con
        // Longstaff-Schwartz, asi que esto es un sanity check direccional: la delta de un put
        // americano/bermuda respecto del spot siempre es <= 0.
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, r, q, sigma, maturity) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let spec = bermuda_json("put", strike, maturity, &[0.25, 0.5, 0.75]);
        let (n_paths, seed) = (20_000, 41);

        let delta =
            payoff_sensitivity_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", "spot", s0, r, q, sigma, n_paths, seed).unwrap();

        assert!(delta.mean < 0.0, "delta={} deberia ser negativa para un put bermuda", delta.mean);
    }

    // Fase 11 (PLAN_PRODUCTS.md §8: "CPU y GPU deben ejecutar el mismo IR y superar tests
    // diferenciales dentro de tolerancia"): mismo contrato (up-and-in barrier, path-dependiente
    // -- ejercita `simulate_gbm_columns_at`/`resolve_trigger_states` de verdad, no solo una call
    // europea de un unico paso), misma seed, `backend="cpu"` vs `backend="gpu"` -- ambos deben
    // ejecutar el MISMO `CompiledPayoff` (la interpretacion pathwise es identica, solo cambia el
    // backend Burn que genera las rutas GBM) y converger al mismo precio dentro de un margen
    // estadistico generoso. Gateado tras `--features gpu`: no compila ni corre en el CI por
    // defecto, igual que el resto de la infraestructura GPU de este crate.
    #[cfg(feature = "gpu")]
    #[test]
    fn cpu_and_gpu_backends_agree_on_the_same_barrier_payoff_within_statistical_tolerance() {
        let _guard = crate::rng_test_lock::LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (s0, strike, barrier, r, q, sigma, maturity) = (100.0, 100.0, 120.0, 0.05, 0.0, 0.2, 1.0);
        let monitoring_times: Vec<f64> = (1..=50).map(|i| i as f64 / 50.0).collect();
        let spec = up_and_in_call_json(barrier, strike, maturity, &monitoring_times);
        let (n_paths, seed) = (200_000, 7);

        let cpu = price_payoff_gbm_q("cpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed, 0.0).unwrap();
        let gpu = price_payoff_gbm_q("gpu", &spec, "EQ.SPOT.XYZ", s0, r, q, sigma, n_paths, seed).unwrap();

        let tolerance = 8.0 * (cpu.std_error + gpu.std_error);
        assert!(
            (cpu.mean - gpu.mean).abs() < tolerance,
            "cpu={} (se={}) gpu={} (se={}) tol={tolerance}",
            cpu.mean,
            cpu.std_error,
            gpu.mean,
            gpu.std_error
        );
    }
}
