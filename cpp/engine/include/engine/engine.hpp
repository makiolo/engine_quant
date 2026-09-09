#pragma once

#include <cstdint>

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

} // namespace engine
