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
// §7.15: medidas "PV"/"DV01" de ENGINE.CALC, ver engine/measure.hpp). Siempre en CPU: una
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

} // namespace engine
