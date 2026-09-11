"""Tests del binding Python del registry (PLAN.md §5.4/§7.6, Fase 3, §6; API de ENGINE.CALC
en PLAN.md §7.15): equivalente en Python de `cpp/engine/tests/test_registry.cpp` (mismo
wiring: Registries/register_builtins vía `engine.Engine`, Registry<T>::create vía
`create_model`/`create_product`, engine::calc vía `Engine.calc`), para confirmar que el
binding nanobind expone la misma API pública que el registry C++ sin reimplementar su lógica.
No repite la validación numérica fina que ya cubren Rust (PLAN.md §5.6 capas 1-2) ni el propio
test_registry.cpp -- ver test_calc.py para el valor de referencia exacto.
"""

import sys
from pathlib import Path

# El módulo compilado (engine.pyd) vive en el directorio de build de CMake, igual que en
# test_smoke.py (PLAN.md §7.1).
if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def _hull_white_params():
    return {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02}


def _hull_white_2f_params():
    # Segundo modelo del motor (PLAN.md §7.16, G2++): mismos parámetros de referencia que
    # cpp/engine/tests/test_registry.cpp::hull_white_2f_params().
    return {"a": 0.1, "b": 0.2, "sigma": 0.01, "eta": 0.012, "rho": -0.7, "r0": 0.03}


def _par_irs_5y_params():
    return {
        "notional": 1_000_000.0,
        "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
        "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
    }


def _market_with_credit(hazard_rate=0.0, recovery_rate=0.0):
    return engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02], hazard_rate=hazard_rate, recovery_rate=recovery_rate)


def _golden_pricing(n_paths=5000, seed=7):
    return engine.PricingContext({"pricing_date": 0.0, "n_paths": float(n_paths), "n_steps": 208.0, "seed": float(seed)})


def _cpu_execution():
    return engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})


def test_register_builtins_populates_all_registries():
    eng = engine.Engine()
    assert "HullWhite1F" in eng.list_models()
    assert "HullWhite2F" in eng.list_models()
    assert "IRSwap" in eng.list_products()
    # PLAN_REAPI.md §6 Fase 3: calc_measure_names() ya no es una tabla curada cerrada de 5
    # nombres -- es Registry<IMeasure>.list() ("ExposureProfile" incluido, antes inalcanzable
    # como nombre de CALC) más los dos alias heredados que no son un tipo registrado propio.
    assert set(eng.list_measures()) == {"PV", "DV01", "ExposureProfile", "ExpectedExposure", "PFE95", "UnilateralCVA"}


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

    result = eng.calc(
        product, ["ExpectedExposure", "PFE95"], model, _market_with_credit(), _golden_pricing(), _cpu_execution()
    )

    ee = result["ExpectedExposure"]
    pfe = result["PFE95"]
    assert len(ee.primary) == len(pfe.primary)
    for ee_i, pfe_i in zip(ee.primary, pfe.primary):
        assert ee_i >= 0.0
        assert pfe_i >= ee_i


def test_unilateral_cva_is_positive_for_nonzero_hazard_rate():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())

    result = eng.calc(
        product, ["UnilateralCVA"], model, _market_with_credit(0.02, 0.4), _golden_pricing(), _cpu_execution()
    )

    cva = result["UnilateralCVA"]
    assert cva.has_scalar
    assert cva.scalar > 0.0


def test_unilateral_cva_is_zero_when_hazard_rate_is_zero():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())

    result = eng.calc(
        product, ["UnilateralCVA"], model, _market_with_credit(0.0, 0.4), _golden_pricing(), _cpu_execution()
    )

    cva = result["UnilateralCVA"]
    assert cva.has_scalar
    assert abs(cva.scalar) < 1e-9


def test_exposure_profile_2f_is_non_negative_and_pfe_dominates_ee():
    # Interfaz homogénea (PLAN.md §7.16): mismo Engine.calc, mismas medidas, solo cambia el
    # nombre/params pasados a create_model -- HullWhite2F en vez de HullWhite1F.
    eng = engine.Engine()
    model = eng.create_model("HullWhite2F", _hull_white_2f_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())

    result = eng.calc(
        product, ["ExpectedExposure", "PFE95"], model, _market_with_credit(), _golden_pricing(), _cpu_execution()
    )

    ee = result["ExpectedExposure"]
    pfe = result["PFE95"]
    assert len(ee.primary) == len(pfe.primary)
    for ee_i, pfe_i in zip(ee.primary, pfe.primary):
        assert ee_i >= 0.0
        assert pfe_i >= ee_i


def test_unilateral_cva_2f_is_positive_for_nonzero_hazard_rate():
    eng = engine.Engine()
    model = eng.create_model("HullWhite2F", _hull_white_2f_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())

    result = eng.calc(
        product, ["UnilateralCVA"], model, _market_with_credit(0.02, 0.4), _golden_pricing(), _cpu_execution()
    )

    cva = result["UnilateralCVA"]
    assert cva.has_scalar
    assert cva.scalar > 0.0


def test_pv_and_dv01_2f_of_a_par_swap():
    eng = engine.Engine()
    model = eng.create_model("HullWhite2F", _hull_white_2f_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())

    result = eng.calc(
        product, ["PV", "DV01"], model, _market_with_credit(), _golden_pricing(1, 1), _cpu_execution()
    )

    assert abs(result["PV"].scalar) < 1e-6
    assert result["DV01"].scalar > 0.0


def test_calc_rejects_unknown_measure_name():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())

    try:
        eng.calc(product, ["NoExiste"], model, _market_with_credit(), _golden_pricing(), _cpu_execution())
        assert False, "se esperaba ValueError"
    except ValueError:
        pass


def test_calc_rejects_swapped_model_and_product():
    # Equivalente Python de MeasureRejectsWrongProductType (test_registry.cpp): en C++ el
    # rechazo lo hace un dynamic_cast dentro de IMeasure::evaluate (std::invalid_argument);
    # en Python, nanobind ya rechaza el tipo en la frontera antes de llegar a calc() (comprobación
    # de tipos más temprana, no un defecto del binding).
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())

    try:
        eng.calc(model, ["ExpectedExposure"], product, _market_with_credit(), _golden_pricing(), _cpu_execution())
        assert False, "se esperaba TypeError"
    except TypeError:
        pass


if __name__ == "__main__":
    test_register_builtins_populates_all_registries()
    test_create_unknown_model_raises_index_error()
    test_exposure_profile_is_non_negative_and_pfe_dominates_ee()
    test_unilateral_cva_is_positive_for_nonzero_hazard_rate()
    test_unilateral_cva_is_zero_when_hazard_rate_is_zero()
    test_exposure_profile_2f_is_non_negative_and_pfe_dominates_ee()
    test_unilateral_cva_2f_is_positive_for_nonzero_hazard_rate()
    test_pv_and_dv01_2f_of_a_par_swap()
    test_calc_rejects_unknown_measure_name()
    test_calc_rejects_swapped_model_and_product()
    print("OK: tests del registry Python (equivalente a test_registry.cpp) pasaron")
