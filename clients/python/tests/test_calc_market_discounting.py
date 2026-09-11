"""Sanity check explicitamente pedido por PLAN_REAPI.md §6 Fase 4: PV/DV01 pasan a descontar
por la curva de `MarketSnapshot` (antes: modelo Hull-White). Equivalente Python de
`cpp/engine/tests/test_registry.cpp::Calc.ParSwapWithExplicitParRateFromAMultiPillarCurveIsZero`
/`Calc.ParSwapViaUseParRateMatchesExplicitParRateOnANonFlatCurve` -- a diferencia de
`test_calc.py` (mercado de 1 pillar, curva plana por extrapolación), aquí la curva tiene forma
real (no plana), así que "PV de un swap par ~ 0" no es una tautología del caso trivial.
"""

import math
import sys

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def _hull_white_params():
    return {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02}


def _upward_sloping_market():
    return engine.MarketSnapshot(pillars=[1.0, 2.0, 3.0, 4.0, 5.0], zero_rates=[0.018, 0.019, 0.020, 0.0205, 0.021])


def _deterministic_pricing():
    return engine.PricingContext({"pricing_date": 0.0, "n_paths": 1.0, "n_steps": 208.0, "seed": 1.0})


def _cpu_execution():
    return engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})


def _par_rate_from_market(market, payment_times, accruals):
    numerator = market.discount_factor(0.0) - market.discount_factor(payment_times[-1])
    denominator = sum(tau * market.discount_factor(t) for t, tau in zip(payment_times, accruals))
    return numerator / denominator


def test_par_swap_with_explicit_par_rate_from_a_multi_pillar_curve_is_zero():
    eng = engine.Engine()
    market = _upward_sloping_market()
    payment_times = [1.0, 2.0, 3.0, 4.0, 5.0]
    accruals = [1.0, 1.0, 1.0, 1.0, 1.0]
    par_rate = _par_rate_from_market(market, payment_times, accruals)

    product = eng.create_product(
        "IRSwap",
        {"notional": 1_000_000.0, "fixed_rate": par_rate, "payment_times": payment_times, "accruals": accruals},
    )
    model = eng.create_model("HullWhite1F", _hull_white_params())
    pricing = _deterministic_pricing()
    execution = _cpu_execution()

    result = eng.calc(product, ["PV"], model, market, pricing, execution)
    assert result["PV"].has_scalar
    assert math.isclose(result["PV"].scalar, 0.0, abs_tol=1e-6)


def test_bucketed_dv01_sums_to_the_parallel_dv01():
    # PLAN_REAPI.md §6 Fase 5: DV01(bucketed=True) bumpea cada pillar individualmente -- la
    # suma de los deltas por pillar debe coincidir con el DV01 "parcial" (bump paralelo).
    eng = engine.Engine()
    market = _upward_sloping_market()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product(
        "IRSwap",
        {
            "notional": 1_000_000.0,
            "fixed_rate": 0.02,
            "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
            "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
        },
    )
    pricing = _deterministic_pricing()
    execution = _cpu_execution()

    parallel_dv01 = eng.calc(product, [("DV01", {})], model, market, pricing, execution)["DV01"].scalar
    bucketed = eng.calc(product, [("DV01", {"bucketed": True})], model, market, pricing, execution)["DV01"]

    assert not bucketed.has_scalar
    assert len(bucketed.times) == len(market.pillars)
    assert len(bucketed.primary) == len(market.pillars)
    assert math.isclose(sum(bucketed.primary), parallel_dv01, abs_tol=1e-6)


def test_par_swap_via_use_par_rate_matches_explicit_par_rate_on_a_non_flat_curve():
    eng = engine.Engine()
    market = _upward_sloping_market()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    pricing = _deterministic_pricing()
    execution = _cpu_execution()

    par_product = eng.create_product(
        "IRSwap",
        {
            "notional": 1_000_000.0,
            "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
            "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
        },  # fixed_rate omitido -- use_par_rate()==True
    )

    result = eng.calc(par_product, ["PV", "DV01"], model, market, pricing, execution)
    assert math.isclose(result["PV"].scalar, 0.0, abs_tol=1e-6)
    assert result["DV01"].scalar > 0.0  # swap pagador: > 0 pase lo que pase con la forma de la curva


if __name__ == "__main__":
    test_par_swap_with_explicit_par_rate_from_a_multi_pillar_curve_is_zero()
    test_bucketed_dv01_sums_to_the_parallel_dv01()
    test_par_swap_via_use_par_rate_matches_explicit_par_rate_on_a_non_flat_curve()
    print("OK: tests de PV/DV01 por curva de mercado (multi-pillar) pasaron")
