"""Tests de `Engine.price`/`price_batch` aceptando medidas con configuración (PLAN_REAPI.md §6
Fase 3, propuesta 3): tuplas `(nombre, params)` conviviendo con strings "pelados" en la misma
llamada, y `DV01(bump=...)` dando un resultado distinto según el bump -- equivalente Python de
`cpp/engine/tests/test_registry.cpp::Price.Dv01BumpIsConfigurableViaMeasureSpec`.

PLAN_IMPROVE_NOTEBOOK2.md Fase 3 (opción (b), `MeasureSpec::alias`): antes de esa fase, dos
entradas con el mismo `measure_name` en la misma llamada a `Engine.price(...)` colisionaban EN
SILENCIO en el dict de salida (la ultima ganaba) -- friccion #5 del plan, el motivo exacto por el
que pedir varias `Greek` (todas con `measure_name == "Greek"`) en una sola llamada era inutil.
Desde esa fase, esa colision lanza `ValueError` explicito (nunca se pisa en silencio) y una
tupla de 3 elementos `(nombre, params, alias)` permite distinguirlas -- ver
`test_duplicate_measure_names_without_alias_raise_instead_of_silently_colliding` y
`test_alias_lets_two_greek_entries_coexist_in_the_same_price_call` mas abajo.
"""

import math
import sys

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import pytest  # noqa: E402

import engine  # noqa: E402


def _hull_white_params():
    return {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02}


def _irs_5y_params(notional, fixed_rate):
    return {
        "notional": notional,
        "fixed_rate": fixed_rate,
        "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
        "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
    }


def _market():
    return engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02])


def _deterministic_pricing():
    return engine.PricingContext({"pricing_date": 0.0, "n_paths": 1.0, "n_steps": 208.0, "seed": 1.0})


def _cpu_execution():
    return engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})


def test_price_accepts_tuples_and_plain_strings_in_the_same_call():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02))
    market = _market()
    pricing = _deterministic_pricing()
    execution = _cpu_execution()

    result = eng.price(product, ["PV", ("DV01", {"bump": 0.0002})], model, market, pricing, execution)
    assert set(result.keys()) == {"PV", "DV01"}
    assert result["PV"].has_scalar
    assert result["DV01"].has_scalar


def test_dv01_bump_changes_the_scalar_proportionally():
    # PLAN_REAPI.md §6 Fase 4: DV01 es bump-and-reval sobre la curva (no ya d(NPV)/d(r0) exacto
    # vía autodiff) -- duplicar el bump duplica el DV01 solo APROXIMADAMENTE (hay una
    # convexidad de segundo orden real en exp(-zero_rate(t)*t)), por eso rel_tol y no abs_tol.
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02))
    market = _market()
    pricing = _deterministic_pricing()
    execution = _cpu_execution()

    default_only = eng.price(product, [("DV01", {})], model, market, pricing, execution)["DV01"].scalar
    doubled = eng.price(product, [("DV01", {"bump": 0.0002})], model, market, pricing, execution)["DV01"].scalar

    assert default_only != doubled
    assert math.isclose(doubled, default_only * 2.0, rel_tol=0.01)


def test_duplicate_measure_names_without_alias_raise_instead_of_silently_colliding():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 3: pedir dos entradas que resuelven al MISMO nombre de
    # salida ("DV01" dos veces, sin alias) en la misma llamada a Engine.price(...) solia pisarse
    # en silencio en el dict de salida (el ultimo ganaba, ver el comentario retirado de este
    # mismo test arriba) -- ahora es un error explicito, nunca un overwrite silencioso.
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02))
    market = _market()
    pricing = _deterministic_pricing()
    execution = _cpu_execution()

    with pytest.raises(Exception, match="mismo nombre de salida 'DV01'"):
        eng.price(product, [("DV01", {}), ("DV01", {"bump": 0.0002})], model, market, pricing, execution)


def test_alias_lets_two_dv01_entries_coexist_in_the_same_price_call():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 3 (opcion (b)): la tupla de 3 elementos (nombre, params,
    # alias) evita la colision de arriba -- cada entrada aparece bajo su propio alias en el
    # dict de salida, con el MISMO valor numerico que pedirlas por separado (alias es puramente
    # de presentacion, no cambia el calculo).
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02))
    market = _market()
    pricing = _deterministic_pricing()
    execution = _cpu_execution()

    default_only = eng.price(product, [("DV01", {})], model, market, pricing, execution)["DV01"].scalar
    doubled = eng.price(product, [("DV01", {"bump": 0.0002})], model, market, pricing, execution)["DV01"].scalar

    result = eng.price(
        product,
        [("DV01", {}, "dv01_default"), ("DV01", {"bump": 0.0002}, "dv01_wide")],
        model, market, pricing, execution,
    )
    assert set(result.keys()) == {"dv01_default", "dv01_wide"}
    assert math.isclose(result["dv01_default"].scalar, default_only, abs_tol=1e-9)
    assert math.isclose(result["dv01_wide"].scalar, doubled, abs_tol=1e-9)


def test_price_resolves_measure_name_directly_from_the_registry():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02))
    market = _market()
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 5000.0, "n_steps": 208.0, "seed": 7.0})
    execution = _cpu_execution()

    result = eng.price(product, ["ExposureProfile"], model, market, pricing, execution)
    profile = result["ExposureProfile"]
    assert len(profile.primary) == 5
    assert len(profile.secondary) == 5
    for ee_i, pfe_i in zip(profile.primary, profile.secondary):
        assert pfe_i >= ee_i


if __name__ == "__main__":
    test_price_accepts_tuples_and_plain_strings_in_the_same_call()
    test_dv01_bump_changes_the_scalar_proportionally()
    test_duplicate_measure_names_without_alias_raise_instead_of_silently_colliding()
    test_alias_lets_two_dv01_entries_coexist_in_the_same_price_call()
    test_price_resolves_measure_name_directly_from_the_registry()
    print("OK: tests de Engine.price con MeasureSpec (tuplas (nombre, params[, alias])) pasaron")
