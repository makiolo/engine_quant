"""Tests de `engine_typed.TradeSpec`/`IRSwap` (PLAN_REAPI.md §6 Fase 1): construcción,
rechazo de `fixed_rate` ausente, `IRSwap.par(...)`, `to_params()` y round-trip contra el
`Engine` real -- mismo caso base que `clients/python/tests/test_price.py`.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import pydantic  # noqa: E402

import engine  # noqa: E402
import engine_typed as q  # noqa: E402


def test_irswap_requires_fixed_rate():
    try:
        q.IRSwap(notional=1_000_000.0, payment_times=[1.0, 2.0], accruals=[1.0, 1.0])
        assert False, "se esperaba ValidationError"
    except pydantic.ValidationError:
        pass


def test_irswap_par_constructor_sets_sentinel():
    trade = q.IRSwap.par(notional=1_000_000.0, payment_times=[1.0, 2.0], accruals=[1.0, 1.0])
    assert trade.fixed_rate == q.PAR


def test_irswap_to_params_omits_fixed_rate_for_par():
    trade = q.IRSwap.par(notional=1_000_000.0, payment_times=[1.0, 2.0], accruals=[1.0, 1.0])
    params = trade.to_params()
    assert "fixed_rate" not in params
    assert params["notional"] == 1_000_000.0


def test_irswap_to_params_includes_explicit_fixed_rate():
    trade = q.IRSwap(notional=1_000_000.0, fixed_rate=0.02, payment_times=[1.0, 2.0], accruals=[1.0, 1.0])
    params = trade.to_params()
    assert params["fixed_rate"] == 0.02


def test_irswap_is_immutable():
    trade = q.IRSwap(notional=1_000_000.0, fixed_rate=0.02, payment_times=[1.0], accruals=[1.0])
    try:
        trade.notional = 2_000_000.0
        assert False, "se esperaba ValidationError (frozen)"
    except pydantic.ValidationError:
        pass


def test_irswap_to_params_feeds_the_real_engine():
    eng = engine.Engine()
    trade = q.IRSwap.par(
        notional=1_000_000.0,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0],
        accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    product = eng.create_product(trade.product_type, trade.to_params())
    assert product.type_name == "IRSwap"

    model = eng.create_model("HullWhite1F", {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02})
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 1.0, "n_steps": 208.0, "seed": 1.0})
    execution = engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})

    result = eng.price(product, ["PV"], model, market, pricing, execution)
    assert result["PV"].has_scalar  # swap par: PV ~ 0, ya cubierto por test_price.py


if __name__ == "__main__":
    test_irswap_requires_fixed_rate()
    test_irswap_par_constructor_sets_sentinel()
    test_irswap_to_params_omits_fixed_rate_for_par()
    test_irswap_to_params_includes_explicit_fixed_rate()
    test_irswap_is_immutable()
    test_irswap_to_params_feeds_the_real_engine()
    print("OK: tests de engine_typed.IRSwap pasaron")
