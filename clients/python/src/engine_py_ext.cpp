#include <nanobind/nanobind.h>

#include "engine/engine.hpp"

namespace nb = nanobind;

// Fase 0: cadena de humo del pipeline de build (PLAN.md §7.1).
// El binding 1:1 con la API pública de la capa C++ llega en Fase 3 (PLAN.md §6); mientras
// tanto se exponen aquí también las funciones de cadena de humo ampliada (Hull-White/IRS/
// exposición/CVA/AAD sobre Burn), no la API definitiva del motor.
NB_MODULE(engine, m) {
    m.def("ping", &engine::ping);

    m.def(
        "hull_white_zero_coupon_bond",
        &engine::hull_white_zero_coupon_bond,
        nb::arg("a"),
        nb::arg("b"),
        nb::arg("sigma"),
        nb::arg("r0"),
        nb::arg("t"),
        nb::arg("maturity")
    );

    m.def(
        "hull_white_zero_coupon_bond_delta_r0",
        &engine::hull_white_zero_coupon_bond_delta_r0,
        nb::arg("a"),
        nb::arg("b"),
        nb::arg("sigma"),
        nb::arg("r0"),
        nb::arg("t"),
        nb::arg("maturity")
    );

    m.def(
        "irs_unilateral_cva_5y",
        &engine::irs_unilateral_cva_5y,
        nb::arg("a"),
        nb::arg("b"),
        nb::arg("sigma"),
        nb::arg("r0"),
        nb::arg("notional"),
        nb::arg("hazard_rate"),
        nb::arg("recovery_rate"),
        nb::arg("n_paths"),
        nb::arg("seed")
    );
}
