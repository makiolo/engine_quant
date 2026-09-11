"""Tests de Engine.price (PLAN.md §7.15): equivalente en Python de las suites `Registry`/`Price`
de `cpp/engine/tests/test_registry.cpp` que ejercitan PV/DV01/ExpectedExposure/PFE95/
UnilateralCVA a través del nuevo ENGINE.PRICE. El valor de referencia exacto de UnilateralCVA y
las series de ExpectedExposure/PFE95 son los mismos que fija test_registry.cpp (mismo caso,
misma semilla) -- ver ese fichero y clients/excel/README.md ("Verificación manual") para el
resto de clientes que deben reproducir exactamente este número.
"""

import math
import sys

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


def _irs_5y_params(notional, fixed_rate):
    # Con fixed_rate explicito (PLAN.md §7.19): price_batch/price_many no soportan
    # use_par_rate, a diferencia de price().
    return {
        "notional": notional,
        "fixed_rate": fixed_rate,
        "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
        "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
    }


def _irs_3y_params(notional, fixed_rate):
    return {
        "notional": notional,
        "fixed_rate": fixed_rate,
        "payment_times": [1.0, 2.0, 3.0],
        "accruals": [1.0, 1.0, 1.0],
    }


def _market_with_credit(hazard_rate=0.0, recovery_rate=0.0):
    return engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02], hazard_rate=hazard_rate, recovery_rate=recovery_rate)


def _golden_pricing(n_paths=5000, seed=7):
    return engine.PricingContext({"pricing_date": 0.0, "n_paths": float(n_paths), "n_steps": 208.0, "seed": float(seed)})


def _cpu_execution():
    return engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})


def test_price_computes_the_full_batch_in_one_call():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    market = _market_with_credit(0.02, 0.4)
    pricing = _golden_pricing()
    execution = _cpu_execution()

    result = eng.price(
        product, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"], model, market, pricing, execution
    )

    assert set(result.keys()) == {"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"}

    ee = result["ExpectedExposure"]
    pfe = result["PFE95"]
    assert len(ee.primary) == 5  # fechas de reseteo auto-derivadas: 0,1,2,3,4
    assert len(pfe.primary) == 5
    for ee_i, pfe_i in zip(ee.primary, pfe.primary):
        assert pfe_i >= ee_i


def test_present_value_of_a_par_swap_is_near_zero():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    market = _market_with_credit()
    pricing = _golden_pricing(n_paths=1, seed=1)  # PV es determinista
    execution = _cpu_execution()

    result = eng.price(product, ["PV"], model, market, pricing, execution)

    pv = result["PV"]
    assert pv.has_scalar
    assert math.isclose(pv.scalar, 0.0, abs_tol=1e-6)


def test_dv01_of_a_payer_swap_is_positive():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    market = _market_with_credit()
    pricing = _golden_pricing(n_paths=1, seed=1)
    execution = _cpu_execution()

    result = eng.price(product, ["DV01"], model, market, pricing, execution)

    dv01 = result["DV01"]
    assert dv01.has_scalar
    assert dv01.scalar > 0.0


def test_exposure_profile_matches_golden_value():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    market = _market_with_credit(0.0, 0.0)
    pricing = _golden_pricing()
    execution = _cpu_execution()

    result = eng.price(product, ["ExpectedExposure", "PFE95"], model, market, pricing, execution)

    expected_times = [0.0, 1.0, 2.0, 3.0, 4.0]
    expected_ee = [0.0, 12862.61794, 13673.52975, 11957.81610, 7124.10624]
    expected_pfe95 = [0.0, 51009.92088, 53607.17082, 46152.44562, 27535.55727]

    ee = result["ExpectedExposure"]
    pfe = result["PFE95"]
    assert list(ee.times) == expected_times
    for actual, expected in zip(ee.primary, expected_ee):
        assert math.isclose(actual, expected, abs_tol=1.0)
    for actual, expected in zip(pfe.primary, expected_pfe95):
        assert math.isclose(actual, expected, abs_tol=1.0)


def test_unilateral_cva_matches_golden_value():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    market = _market_with_credit(0.02, 0.4)
    pricing = _golden_pricing()
    execution = _cpu_execution()

    result = eng.price(product, ["UnilateralCVA"], model, market, pricing, execution)

    cva = result["UnilateralCVA"]
    assert cva.has_scalar
    assert math.isclose(cva.scalar, 503.6419407799754, abs_tol=1e-6)


def test_price_batch_matches_a_loop_of_scalar_calls_per_trade():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    products = [
        eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02)),
        eng.create_product("IRSwap", _irs_5y_params(2_500_000.0, 0.015)),
        eng.create_product("IRSwap", _irs_5y_params(500_000.0, 0.025)),
    ]
    market = _market_with_credit(0.02, 0.4)
    pricing = _golden_pricing()
    execution = _cpu_execution()
    measures = ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"]

    batch = eng.price_batch(products, measures, model, market, pricing, execution)
    assert len(batch) == len(products)

    for i, row in enumerate(batch):
        assert row.trade_index == i
        scalar = eng.price(products[i], measures, model, market, pricing, execution)
        for name in measures:
            if scalar[name].has_scalar:
                assert math.isclose(row.measures[name].scalar, scalar[name].scalar, abs_tol=1e-6)
            else:
                for a, b in zip(row.measures[name].primary, scalar[name].primary):
                    assert math.isclose(a, b, abs_tol=1e-6)


def test_price_batch_rejects_mismatched_calendars():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    products = [
        eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02)),
        eng.create_product("IRSwap", _irs_3y_params(1_000_000.0, 0.02)),
    ]
    market = _market_with_credit()
    pricing = _golden_pricing(n_paths=100, seed=1)
    execution = _cpu_execution()

    try:
        eng.price_batch(products, ["PV"], model, market, pricing, execution)
        assert False, "se esperaba ValueError"
    except ValueError:
        pass


def test_price_batch_rejects_use_par_rate():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    products = [eng.create_product("IRSwap", _par_irs_5y_params())]  # sin fixed_rate
    market = _market_with_credit()
    pricing = _golden_pricing(n_paths=100, seed=1)
    execution = _cpu_execution()

    try:
        eng.price_batch(products, ["PV"], model, market, pricing, execution)
        assert False, "se esperaba ValueError"
    except ValueError:
        pass


def test_price_many_groups_heterogeneous_calendars_and_preserves_order():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product_5y_a = eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02))
    product_3y = eng.create_product("IRSwap", _irs_3y_params(2_000_000.0, 0.018))
    product_5y_b = eng.create_product("IRSwap", _irs_5y_params(3_000_000.0, 0.022))
    market = _market_with_credit(0.02, 0.4)
    pricing = _golden_pricing()
    execution = _cpu_execution()

    # Intercalados a proposito: 5y, 3y, 5y -- dos grupos de calendario, no en bloques contiguos.
    products = [product_5y_a, product_3y, product_5y_b]
    measures = ["PV", "UnilateralCVA"]
    many = eng.price_many(products, measures, model, market, pricing, execution)
    assert len(many) == len(products)

    for i, row in enumerate(many):
        assert row.trade_index == i
        scalar = eng.price(products[i], measures, model, market, pricing, execution)
        for name in measures:
            assert math.isclose(row.measures[name].scalar, scalar[name].scalar, abs_tol=1e-6)


def test_price_grid_computes_trades_times_models_times_markets():
    eng = engine.Engine()
    model_1f = eng.create_model("HullWhite1F", _hull_white_params())
    model_2f = eng.create_model(
        "HullWhite2F", {"a": 0.1, "b": 0.2, "sigma": 0.01, "eta": 0.012, "rho": -0.7, "r0": 0.03}
    )
    products = [
        eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02)),
        eng.create_product("IRSwap", _irs_5y_params(2_000_000.0, 0.018)),
    ]
    market_a = _market_with_credit(0.02, 0.4)
    market_b = _market_with_credit(0.05, 0.3)
    pricing = _golden_pricing()
    execution = _cpu_execution()
    models = [model_1f, model_2f]
    markets = [market_a, market_b]
    measures = ["PV", "UnilateralCVA"]

    grid = eng.price_grid(products, measures, models, markets, pricing, execution)
    assert len(grid) == len(products) * len(models) * len(markets)

    for cell in grid:
        scalar = eng.price(
            products[cell.trade_index], measures, models[cell.model_index], markets[cell.market_index], pricing, execution
        )
        for name in measures:
            assert math.isclose(cell.measures[name].scalar, scalar[name].scalar, abs_tol=1e-6)


def test_price_grid_rejects_empty_models_or_markets():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _irs_5y_params(1_000_000.0, 0.02))
    market = _market_with_credit()
    pricing = _golden_pricing(n_paths=100, seed=1)
    execution = _cpu_execution()

    try:
        eng.price_grid([product], ["PV"], [], [market], pricing, execution)
        assert False, "se esperaba ValueError"
    except ValueError:
        pass
    try:
        eng.price_grid([product], ["PV"], [model], [], pricing, execution)
        assert False, "se esperaba ValueError"
    except ValueError:
        pass


def test_execution_context_resolves_auto_backend():
    execution = engine.ExecutionContext({"backend": "auto"})
    expected = "gpu" if engine.is_gpu_backend_available() else "cpu"
    assert execution.backend == expected


def test_execution_context_rejects_unknown_backend():
    try:
        engine.ExecutionContext({"backend": "quantum"})
        assert False, "se esperaba ValueError"
    except ValueError:
        pass


if __name__ == "__main__":
    test_price_computes_the_full_batch_in_one_call()
    test_present_value_of_a_par_swap_is_near_zero()
    test_dv01_of_a_payer_swap_is_positive()
    test_exposure_profile_matches_golden_value()
    test_unilateral_cva_matches_golden_value()
    test_price_batch_matches_a_loop_of_scalar_calls_per_trade()
    test_price_batch_rejects_mismatched_calendars()
    test_price_batch_rejects_use_par_rate()
    test_price_many_groups_heterogeneous_calendars_and_preserves_order()
    test_price_grid_computes_trades_times_models_times_markets()
    test_price_grid_rejects_empty_models_or_markets()
    test_execution_context_resolves_auto_backend()
    test_execution_context_rejects_unknown_backend()
    print("OK: tests de Engine.price pasaron")
