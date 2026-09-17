#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// Fase 0: cadena de humo del pipeline de build (PLAN.md §7.1).
// El registry, IModel/IProduct/IMeasure y el bootstrap llegan en Fase 2 (PLAN.md §5.4).
double ping();

// Cadena de humo ampliada (PLAN.md §7.1): ejercita Hull-White 1F + IRS + exposición/CVA +
// AAD, ya sobre Burn (PLAN.md §5.1, §5.3), a través de las cuatro capas del pipeline
// (Rust -> cxx -> C++ -> nanobind -> Python). No son la API definitiva del motor — eso es
// el registry de Fase 2 — solo confirman que el pipeline sigue funcionando de punta a
// punta con lógica de negocio real detrás, no solo `ping()`.
double hull_white_zero_coupon_bond(double a, double b, double sigma, double r0, double t, double maturity);
double hull_white_zero_coupon_bond_delta_r0(double a, double b, double sigma, double r0, double t, double maturity);

// Equivalente de dos factores (PLAN.md §7.16/§7.18): precio del bono cero-cupón de
// HullWhite2F/G2++ con los dos factores latentes en su valor inicial (x_0 = y_0 = 0, ver
// engine_core::models::hull_white_2f). Usado por
// engine::MarketSnapshot::synthetic_from_hull_white_2f para fabricar un mercado sin depender
// de datos reales, igual que hull_white_zero_coupon_bond ya hace para HullWhite1F.
double hull_white_2f_zero_coupon_bond(double a, double b, double sigma, double eta, double rho, double r0, double maturity);
double irs_unilateral_cva_5y(
    double a,
    double b,
    double sigma,
    double r0,
    double notional,
    double hazard_rate,
    double recovery_rate,
    std::uint64_t n_paths,
    std::uint64_t seed
);

// API generalizada de Fase 2 (PLAN.md §5.4, §6): a diferencia de irs_unilateral_cva_5y
// (cadena de humo de Fase 1, IRS 5y anual fijo), separa el cálculo del perfil de exposición
// del cálculo del CVA a partir de ese perfil, para que measure.hpp pueda componerlas.
struct ExposureProfile {
    std::vector<double> times;
    std::vector<double> ee;
    std::vector<double> pfe_95;
};

// Perfil de exposición (EE/PFE) de un IRS bajo Hull-White 1F, vía Monte Carlo (PLAN.md §5.2).
// notional/fixed_rate/start describen el IRS; si use_par_rate es true, fixed_rate se ignora y
// el tipo fijo se calcula a la par en `start` (igual que hace irs_unilateral_cva_5y, pero
// aquí el IRS es arbitrario en vez de fijo a 5 años anuales). `backend` ("cpu"/"gpu") es un
// parámetro explícito (PLAN.md §7.15) — ya no hay estado global de backend (§7.12, retirado);
// `n_steps` (PLAN.md §7.15, `PricingContext::n_steps`) sustituye la malla semanal que se
// calculaba antes internamente.
ExposureProfile irs_hull_white_exposure_profile(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate,
    double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
);

// CVA unilateral (hazard rate plana, recovery rate constante) a partir de un perfil EE ya
// calculado (times/ee, mismo largo) — separa el cálculo del perfil del cálculo del CVA para
// que la capa de medidas (measure.hpp) pueda componerlas. Mismo `backend` explícito que
// irs_hull_white_exposure_profile.
double unilateral_cva_from_exposure(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
);

// `true` si el core Rust se compiló con soporte GPU (feature `gpu`, ver ENGINE_QUANT_ENABLE_GPU
// en el CMakeLists.txt raíz) — independientemente del backend que se pida en cada llamada.
// Usado por `ExecutionContext` para resolver `"auto"` y para rechazar `"gpu"` con un mensaje
// claro si este build no lo soporta.
bool is_gpu_backend_available();

// NPV determinista (sin Monte Carlo) del IRS a t=0 y su derivada respecto a r0 (PLAN.md
// §7.15: medidas "PV"/"DV01" de ENGINE.PRICE, ver engine/measure.hpp). Siempre en CPU: una
// única evaluación no se beneficia de GPU.
double irs_hull_white_npv(
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);
double irs_hull_white_npv_delta_r0(
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);

// Las cuatro derivadas de primer orden del NPV determinista respecto de a/b/sigma/r0, en una
// única pasada AAD reverse-mode (PLAN_GREEKS.md §5.2/§11 Fase 7) -- ver
// `engine_core::api::irs_hull_white_npv_all_greeks`. Generaliza `irs_hull_white_npv_delta_r0`
// (que sigue existiendo sin cambios, fachada retrocompatible).
struct HullWhite1FGreeks {
    double d_a = 0.0;
    double d_b = 0.0;
    double d_sigma = 0.0;
    double d_r0 = 0.0;
};
HullWhite1FGreeks irs_hull_white_npv_all_greeks(
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);

// Valor + Hessiano 4x4 completo (10 pares) del NPV determinista de Hull-White 1F
// (PLAN_BACKWARD.md §9 Fase 2), via Dual2/HyperDual NUEVOS (forward-over-forward cerrado) --
// Burn no anida Autodiff para dar un Hessiano (PLAN_BACKWARD.md §1.2), asi que esta es una
// SEGUNDA implementacion escalar del mismo pricer, verificada en valor/gradiente contra
// HullWhite1FGreeks (ver engine_core::models::hull_white_dual, tests de Rust) antes de confiar en
// su Hessiano. Mismos parametros que irs_hull_white_npv_all_greeks.
struct HullWhite1FHessian {
    double value = 0.0;
    double d_a = 0.0;
    double d_b = 0.0;
    double d_sigma = 0.0;
    double d_r0 = 0.0;
    double d_aa = 0.0;
    double d_bb = 0.0;
    double d_sigmasigma = 0.0;
    double d_r0r0 = 0.0;
    double d_ab = 0.0;
    double d_asigma = 0.0;
    double d_ar0 = 0.0;
    double d_bsigma = 0.0;
    double d_br0 = 0.0;
    double d_sigmar0 = 0.0;
};
HullWhite1FHessian hull_white_1f_hessian(
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);

// Lote homogéneo (PLAN.md §7.17/§7.19): las mismas cuatro medidas de arriba vectorizadas sobre
// N trades del mismo calendario -- notionals/fixed_rates son columnas (un valor por trade), sin
// use_par_rate (cada trade del lote debe traer su fixed_rate explícito). irs_hull_white_npv_
// delta_r0_batch NO es una sola pasada backward() para todo el lote -- ver
// engine_core::api::irs_hull_white_npv_delta_r0_batch (Rust) para el porqué: evita N
// round-trips de FFI/C++/Python/Excel, no las N pasadas backward en sí.
std::vector<double> irs_hull_white_npv_batch(
    double a, double b, double sigma, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);
std::vector<double> irs_hull_white_npv_delta_r0_batch(
    double a, double b, double sigma, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);
std::vector<ExposureProfile> irs_hull_white_exposure_profile_batch(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
);
// `profiles` es el mismo vector que ya devuelve irs_hull_white_exposure_profile_batch -- se
// pasa de vuelta tal cual, sin recalcular el perfil.
std::vector<double> unilateral_cva_from_exposure_batch(
    const std::string& backend,
    double a, double b, double sigma, double r0,
    const std::vector<ExposureProfile>& profiles,
    double hazard_rate, double recovery_rate
);

// Segundo modelo del motor, Hull-White 2 factores (PLAN.md §7.16, G2++): mismas cuatro
// funciones que su equivalente de 1 factor arriba (perfil de exposición, CVA a partir de un
// perfil, NPV determinista y su sensibilidad a r0), con dos parámetros adicionales (`eta`,
// `rho`) y sin `HullWhite1FModel::b` como "nivel de reversión" -- aquí `b` es la velocidad de
// reversión del segundo factor. Mismo shape de resultado (`ExposureProfile`) y misma
// interfaz de `backend` explícito: `engine/measure.hpp` las consume con el mismo código de
// medida que las de 1 factor, solo cambiando cuál de las dos llama según el modelo recibido.
ExposureProfile irs_hull_white_2f_exposure_profile(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate,
    double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
);

double unilateral_cva_from_exposure_2f(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
);

double irs_hull_white_2f_npv(
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);
double irs_hull_white_2f_npv_delta_r0(
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);

// Equivalente 2F de HullWhite1FGreeks/irs_hull_white_npv_all_greeks -- SIN d_rho: `rho` no es
// un tensor diferenciable en `HullWhite2F` (parámetro `f64` plano del lado Rust, ver
// `engine_core::api::HullWhite2FGreeks`), así que no hay gradiente reverse-mode que leer para
// él con la implementación actual del modelo. Sigue siendo una `RiskFactor::ModelParameter`
// válida vía bump-and-reval (la tabla de capacidades de `engine::greeks` no declara
// `aad_supported` para `rho`, ver greeks.cpp).
struct HullWhite2FGreeks {
    double d_a = 0.0;
    double d_b = 0.0;
    double d_sigma = 0.0;
    double d_eta = 0.0;
    double d_r0 = 0.0;
};
HullWhite2FGreeks irs_hull_white_2f_npv_all_greeks(
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);

// Valor + Hessiano 5x5 completo (15 pares) del NPV determinista de Hull-White 2F
// (PLAN_BACKWARD.md §9 Fase 3), mismo mecanismo que HullWhite1FHessian (Dual2/HyperDual nuevos,
// forward-over-forward cerrado, no AAD reverse-mode de Burn -- Burn no anida
// Autodiff<Autodiff<_>>, PLAN_BACKWARD.md §1.2). No incluye d_rho ni entradas cruzadas con rho --
// rho es un f64 plano no diferenciable, mismo criterio que HullWhite2FGreeks sin d_rho.
struct HullWhite2FHessian {
    double value = 0.0;
    double d_a = 0.0;
    double d_b = 0.0;
    double d_sigma = 0.0;
    double d_eta = 0.0;
    double d_r0 = 0.0;
    double d_aa = 0.0;
    double d_bb = 0.0;
    double d_sigmasigma = 0.0;
    double d_etaeta = 0.0;
    double d_r0r0 = 0.0;
    double d_ab = 0.0;
    double d_asigma = 0.0;
    double d_aeta = 0.0;
    double d_ar0 = 0.0;
    double d_bsigma = 0.0;
    double d_beta = 0.0;
    double d_br0 = 0.0;
    double d_sigmaeta = 0.0;
    double d_sigmar0 = 0.0;
    double d_etar0 = 0.0;
};

HullWhite2FHessian hull_white_2f_hessian(
    double a, double b, double sigma, double eta, double rho, double r0,
    double notional, double fixed_rate, bool use_par_rate, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);

// Equivalentes de lote de las cuatro funciones 2F de arriba -- ver las versiones de 1 factor
// para el porqué de cada una (PLAN.md §7.19).
std::vector<double> irs_hull_white_2f_npv_batch(
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);
std::vector<double> irs_hull_white_2f_npv_delta_r0_batch(
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
);
std::vector<ExposureProfile> irs_hull_white_2f_exposure_profile_batch(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<double>& notionals, const std::vector<double>& fixed_rates, double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
);
std::vector<double> unilateral_cva_from_exposure_2f_batch(
    const std::string& backend,
    double a, double b, double sigma, double eta, double rho, double r0,
    const std::vector<ExposureProfile>& profiles,
    double hazard_rate, double recovery_rate
);

// --- Diagnostico de trayectorias Monte Carlo (PLAN_IMPROVE_NOTEBOOK.md Fase 0) --------------
//
// `simulate_paths_gbm_q`/`simulate_paths_gbm_p` NO son una `IMeasure` (nunca pasan por
// `Registry<IMeasure>`, no se llaman desde `engine::price`) -- son una herramienta de
// diagnostico/notebook que expone la matriz completa de trayectorias que
// `engine_core::models::gbm::Gbm`/`GbmP::simulate_at_times` ya calculan por dentro para las
// medidas `Payoff*Q`/`Payoff*P`, en vez de solo el agregado final que esas medidas consumen.
// Deliberadamente fuera de alcance de Excel/C ABI en esta fase (mismo criterio que
// PLAN_GREEKS.md §15 deja fuera el AAD reverse-mode de Excel): el dispatch por tipo de modelo
// (GBM/GBM_P) vive en `Engine::simulate_paths` (Python, `clients/python/src/engine_py_ext.cpp`),
// NO aqui -- estas dos funciones libres toman los parametros ya extraidos del modelo, mismo
// patron que el resto de este fichero (p.ej. `irs_hull_white_exposure_profile` toma
// `a`/`b`/`sigma`/`r0`, no un `HullWhite1FModel`).
//
// Tope duro (documentado tambien en `engine_core::api::SIMULATE_PATHS_MAX_PATHS`/
// `SIMULATE_PATHS_MAX_STEPS`, rust/crates/engine-core/src/api.rs -- la validacion REAL ocurre
// alli, antes de simular una sola ruta; estas constantes son la misma cifra repetida aqui solo
// para que quien lea engine.hpp no tenga que saltar a Rust para conocer el limite):
// 50 000 paths x 500 pasos (~200 MB para la matriz aplanada) -- generoso para un notebook
// interactivo, acotado para no agotar memoria de un proceso normal si alguien pide una malla
// desproporcionada sin darse cuenta.
inline constexpr std::uint64_t kSimulatePathsMaxPaths = 50'000;
inline constexpr std::uint64_t kSimulatePathsMaxSteps = 500;

// Matriz cruda de trayectorias simuladas: `times.size() == n_steps + 1` (incluye `t=0`, `S0`
// repetido sin simular) x `n_paths` rutas. **Orden de aplanado: ROW-MAJOR POR PATH** --
// `paths_flat[path * (n_steps + 1) + step]` es el valor de la ruta `path` en `times[step]` --
// MISMA convencion que `ffi::PathMatrixResult` (Rust, `engine-ffi/src/lib.rs`) y que el
// docstring de `Engine.simulate_paths` (Python): ninguna capa reordena.
struct PathMatrix {
    std::vector<double> times;
    std::vector<double> paths_flat;
    std::uint64_t n_paths = 0;
    std::uint64_t n_steps = 0;
};

// Trayectorias crudas de GBM bajo Q en una malla uniforme [0, maturity] de n_steps intervalos
// (times = [0, dt, 2*dt, ..., maturity], dt = maturity/n_steps) -- lanza std::invalid_argument
// si n_paths/n_steps es 0, si excede el tope duro de arriba, o si maturity no es finito y > 0
// (preflight, ANTES de simular una sola ruta, ver engine_core::api::simulate_paths_gbm_q).
PathMatrix simulate_paths_gbm_q(
    const std::string& backend,
    double s0, double r, double q, double sigma, double maturity,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
);

// Equivalente bajo P (drift fisico `mu`, sin `r`/`q`) de simulate_paths_gbm_q -- ver
// engine_core::api::simulate_paths_gbm_p.
PathMatrix simulate_paths_gbm_p(
    const std::string& backend,
    double s0, double mu, double sigma, double maturity,
    std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
);

// Equivalente de `PathMatrix` para `GbmBasketModel` (PLAN_IMPROVE_NOTEBOOK2.md Fase 4):
// `n_assets` observables correlacionados en vez de uno solo. **Orden de aplanado: ROW-MAJOR POR
// (path, step, asset)** -- `paths_flat[path * (n_steps + 1) * n_assets + step * n_assets +
// asset]` es el valor del activo `asset` de la ruta `path` en `times[step]` -- MISMA convencion
// que `ffi::BasketPathMatrixResult` (Rust) y el docstring de `Engine.simulate_paths` (Python):
// ninguna capa reordena. Elegida para que Python solo necesite `reshape((n_paths, n_steps+1,
// n_assets))`, sin transponer.
struct BasketPathMatrix {
    std::vector<double> times;
    std::vector<double> paths_flat;
    std::uint64_t n_paths = 0;
    std::uint64_t n_steps = 0;
    std::uint64_t n_assets = 0;
};

// Trayectorias crudas de GbmBasket bajo Q en una malla uniforme [0, maturity] de n_steps
// intervalos -- mismo criterio de preflight/tope duro que simulate_paths_gbm_q (ver
// engine_core::api::simulate_paths_gbm_basket_q); `s0`/`r`/`q`/`sigma` uno por activo (define
// n_assets = s0.size()), `correlation_flat` aplanada fila a fila n_assets x n_assets.
BasketPathMatrix simulate_paths_gbm_basket_q(
    const std::string& backend,
    const std::vector<double>& s0, const std::vector<double>& r, const std::vector<double>& q,
    const std::vector<double>& sigma, const std::vector<double>& correlation_flat,
    double maturity, std::uint64_t n_steps, std::uint64_t n_paths, std::uint64_t seed
);

} // namespace engine
