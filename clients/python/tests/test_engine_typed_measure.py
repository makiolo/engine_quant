"""Tests de `engine_typed.Measure` (PLAN_REAPI.md §6 Fase 3): `.to_spec()` produce la tupla
`(nombre, params)` que consume `Engine.calc`, y `DV01(bump=...)` tiene efecto real de punta a
punta.
"""

import math
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402
import engine_typed as q  # noqa: E402


def test_pv_to_spec_has_no_params():
    assert q.PV().to_spec() == ("PV", {})


def test_dv01_to_spec_carries_bump():
    assert q.DV01(bump=0.0002).to_spec() == ("DV01", {"bump": 0.0002, "bucketed": False})
    assert q.DV01().to_spec() == ("DV01", {"bump": 0.0001, "bucketed": False})


def test_dv01_to_spec_carries_bucketed():
    assert q.DV01(bucketed=True).to_spec() == ("DV01", {"bump": 0.0001, "bucketed": True})


def test_dv01_bump_has_a_real_effect_through_the_engine():
    eng = engine.Engine()
    trade = q.IRSwap(
        notional=1_000_000.0, fixed_rate=0.02,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    market = q.Market(pillars=[1.0], zero_rates=[0.02])
    pricing = q.PricingContext(n_paths=1, n_steps=208, seed=1)  # DV01 es determinista
    execution = q.ExecutionContext(backend="cpu")

    product = eng.create_product(trade.product_type, trade.to_params())
    eng_model = eng.create_model(model.model_type, model.to_params())
    eng_market = engine.MarketSnapshot(**market.to_params())
    eng_pricing = engine.PricingContext(pricing.to_params())
    eng_execution = engine.ExecutionContext(execution.to_params())

    default_dv01 = eng.calc(product, [q.DV01().to_spec()], eng_model, eng_market, eng_pricing, eng_execution)["DV01"].scalar
    doubled_dv01 = eng.calc(
        product, [q.DV01(bump=0.0002).to_spec()], eng_model, eng_market, eng_pricing, eng_execution
    )["DV01"].scalar

    # Bump-and-reval (PLAN_REAPI.md §6 Fase 4): aproximadamente proporcional, no exacto (rel_tol).
    assert math.isclose(doubled_dv01, default_dv01 * 2.0, rel_tol=0.01)


def test_dv01_bucketed_sums_to_the_parallel_dv01_through_the_engine():
    # PLAN_REAPI.md §6 Fase 5.
    eng = engine.Engine()
    trade = q.IRSwap(
        notional=1_000_000.0, fixed_rate=0.02,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    market = q.Market(pillars=[1.0, 2.0, 3.0, 4.0, 5.0], zero_rates=[0.018, 0.019, 0.020, 0.0205, 0.021])
    pricing = q.PricingContext(n_paths=1, n_steps=208, seed=1)
    execution = q.ExecutionContext(backend="cpu")

    product = eng.create_product(trade.product_type, trade.to_params())
    eng_model = eng.create_model(model.model_type, model.to_params())
    eng_market = engine.MarketSnapshot(**market.to_params())
    eng_pricing = engine.PricingContext(pricing.to_params())
    eng_execution = engine.ExecutionContext(execution.to_params())

    parallel = eng.calc(product, [q.DV01().to_spec()], eng_model, eng_market, eng_pricing, eng_execution)["DV01"]
    bucketed = eng.calc(product, [q.DV01(bucketed=True).to_spec()], eng_model, eng_market, eng_pricing, eng_execution)["DV01"]

    assert not bucketed.has_scalar
    assert len(bucketed.primary) == len(market.pillars)
    assert math.isclose(sum(bucketed.primary), parallel.scalar, abs_tol=1e-6)


def test_calc_mixes_typed_measures_and_plain_strings():
    eng = engine.Engine()
    trade = q.IRSwap(
        notional=1_000_000.0, fixed_rate=0.02,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    market = q.Market(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    pricing = q.PricingContext(n_paths=1, n_steps=208, seed=1)
    execution = q.ExecutionContext(backend="cpu")

    product = eng.create_product(trade.product_type, trade.to_params())
    eng_model = eng.create_model(model.model_type, model.to_params())
    eng_market = engine.MarketSnapshot(**market.to_params())
    eng_pricing = engine.PricingContext(pricing.to_params())
    eng_execution = engine.ExecutionContext(execution.to_params())

    result = eng.calc(
        product, [q.PV().to_spec(), q.DV01(bump=0.0002).to_spec(), "UnilateralCVA"],
        eng_model, eng_market, eng_pricing, eng_execution,
    )
    assert set(result.keys()) == {"PV", "DV01", "UnilateralCVA"}


if __name__ == "__main__":
    test_pv_to_spec_has_no_params()
    test_dv01_to_spec_carries_bump()
    test_dv01_to_spec_carries_bucketed()
    test_dv01_bump_has_a_real_effect_through_the_engine()
    test_dv01_bucketed_sums_to_the_parallel_dv01_through_the_engine()
    test_calc_mixes_typed_measures_and_plain_strings()
    print("OK: tests de engine_typed.Measure pasaron")
