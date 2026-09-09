#pragma once

#include <cstdint>
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
// aquí el IRS es arbitrario en vez de fijo a 5 años anuales).
ExposureProfile irs_hull_white_exposure_profile(
    double a, double b, double sigma, double r0,
    double notional, double fixed_rate, bool use_par_rate,
    double start,
    const std::vector<double>& payment_times,
    const std::vector<double>& accruals,
    const std::vector<double>& monitoring_times,
    std::uint64_t n_paths, std::uint64_t seed
);

// CVA unilateral (hazard rate plana, recovery rate constante) a partir de un perfil EE ya
// calculado (times/ee, mismo largo) — separa el cálculo del perfil del cálculo del CVA para
// que la capa de medidas (measure.hpp) pueda componerlas.
double unilateral_cva_from_exposure(
    double a, double b, double sigma, double r0,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
);

} // namespace engine
