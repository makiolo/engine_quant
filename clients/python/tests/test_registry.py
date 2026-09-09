"""Tests del binding Python del registry (PLAN.md §5.4/§7.6, Fase 3, §6): equivalente en
Python de `cpp/engine/tests/test_registry.cpp` (mismo wiring: Registries/register_builtins vía
`engine.Engine`, Registry<T>::create vía `create_model`/`create_product`/`create_measure`,
IMeasure::evaluate vía `Measure.evaluate`), para confirmar que el binding nanobind expone
la misma API pública que el registry C++ sin reimplementar su lógica. No repite la validación
numérica fina que ya cubren Rust (PLAN.md §5.6 capas 1-2) ni el propio test_registry.cpp.
"""

import math
import sys
from pathlib import Path

# El módulo compilado (engine.pyd) vive en el directorio de build de CMake, igual que en
# test_smoke.py (PLAN.md §7.1).
if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def _hull_white_params():
    return {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02}


def _par_irs_5y_params():
    return {
        "notional": 1_000_000.0,
        "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
        "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
    }


def test_register_builtins_populates_all_registries():
    eng = engine.Engine()
    assert "HullWhite1F" in eng.list_models()
    assert "IRSwap" in eng.list_products()
    assert "ExposureProfile" in eng.list_measures()
    assert "UnilateralCVA" in eng.list_measures()


def test_create_unknown_model_raises_index_error():
    # Registry<T>::create lanza std::out_of_range en C++; nanobind lo traduce a IndexError.
    eng = engine.Engine()
    try:
        eng.create_model("NoExiste", {})
        assert False, "se esperaba IndexError"
    except IndexError:
        pass


def test_exposure_profile_is_non_negative_and_pfe_dominates_ee():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    measure = eng.create_measure("ExposureProfile")

    result = measure.evaluate(
        model,
        product,
        {"monitoring_times": [0.0, 1.0, 2.0], "n_paths": 5000.0, "seed": 7.0},
    )

    assert len(result.primary) == len(result.secondary)
    for ee, pfe in zip(result.primary, result.secondary):
        assert ee >= 0.0
        assert pfe >= ee


def test_unilateral_cva_is_positive_for_nonzero_hazard_rate():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    measure = eng.create_measure("UnilateralCVA")

    result = measure.evaluate(
        model,
        product,
        {
            "monitoring_times": [0.0, 1.0, 2.0, 3.0],
            "n_paths": 5000.0,
            "seed": 13.0,
            "hazard_rate": 0.02,
            "recovery_rate": 0.4,
        },
    )

    assert result.has_scalar
    assert result.scalar > 0.0


def test_unilateral_cva_is_zero_when_hazard_rate_is_zero():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    measure = eng.create_measure("UnilateralCVA")

    result = measure.evaluate(
        model,
        product,
        {
            "monitoring_times": [0.0, 1.0, 2.0, 3.0],
            "n_paths": 5000.0,
            "seed": 13.0,
            "hazard_rate": 0.0,
            "recovery_rate": 0.4,
        },
    )

    assert result.has_scalar
    assert math.isclose(result.scalar, 0.0, abs_tol=1e-9)


def test_measure_rejects_swapped_model_and_product():
    # Equivalente Python de MeasureRejectsWrongProductType (test_registry.cpp): en C++ el
    # rechazo lo hace un dynamic_cast dentro de IMeasure::evaluate (std::invalid_argument);
    # en Python, nanobind ya rechaza el tipo en la frontera antes de llegar a evaluate()
    # (comprobación de tipos más temprana, no un defecto del binding).
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    measure = eng.create_measure("ExposureProfile")

    try:
        measure.evaluate(product, model, {"monitoring_times": [0.0, 1.0], "n_paths": 100.0, "seed": 1.0})
        assert False, "se esperaba TypeError"
    except TypeError:
        pass


if __name__ == "__main__":
    test_register_builtins_populates_all_registries()
    test_create_unknown_model_raises_index_error()
    test_exposure_profile_is_non_negative_and_pfe_dominates_ee()
    test_unilateral_cva_is_positive_for_nonzero_hazard_rate()
    test_unilateral_cva_is_zero_when_hazard_rate_is_zero()
    test_measure_rejects_swapped_model_and_product()
    print("OK: tests del registry Python (equivalente a test_registry.cpp) pasaron")
