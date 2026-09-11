"""Tests de `engine_typed.HullWhite1F/HullWhite2F/Market/PricingContext/ExecutionContext`
(PLAN_REAPI.md §6 Fase 2): construcción/validación/`to_params()` y round-trip contra el
`Engine` real, sin dicts crudos.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import pydantic  # noqa: E402

import engine  # noqa: E402
import engine_typed as q  # noqa: E402


def test_hull_white_1f_to_params_feeds_the_real_engine():
    eng = engine.Engine()
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    eng_model = eng.create_model(model.model_type, model.to_params())
    assert eng_model.type_name == "HullWhite1F"


def test_hull_white_2f_to_params_feeds_the_real_engine():
    eng = engine.Engine()
    model = q.HullWhite2F(a=0.1, b=0.2, sigma=0.01, eta=0.012, rho=-0.7, r0=0.03)
    eng_model = eng.create_model(model.model_type, model.to_params())
    assert eng_model.type_name == "HullWhite2F"


def test_hull_white_requires_all_fields():
    try:
        q.HullWhite1F(a=0.1, b=0.03, sigma=0.01)  # falta r0
        assert False, "se esperaba ValidationError"
    except pydantic.ValidationError:
        pass


def test_market_rejects_non_increasing_pillars():
    try:
        q.Market(pillars=[2.0, 1.0], zero_rates=[0.02, 0.02])
        assert False, "se esperaba ValidationError"
    except pydantic.ValidationError:
        pass


def test_market_rejects_mismatched_lengths():
    try:
        q.Market(pillars=[1.0, 2.0], zero_rates=[0.02])
        assert False, "se esperaba ValidationError"
    except pydantic.ValidationError:
        pass


def test_market_to_params_feeds_the_real_engine():
    market = q.Market(pillars=[1.0, 2.0], zero_rates=[0.02, 0.021], hazard_rate=0.02, recovery_rate=0.4)
    eng_market = engine.MarketSnapshot(**market.to_params())
    assert list(eng_market.pillars) == [1.0, 2.0]
    assert eng_market.hazard_rate == 0.02


def test_pricing_context_rejects_non_positive_paths():
    try:
        q.PricingContext(n_paths=0, n_steps=10, seed=1)
        assert False, "se esperaba ValidationError"
    except pydantic.ValidationError:
        pass


def test_pricing_context_to_params_feeds_the_real_engine():
    pricing = q.PricingContext(n_paths=100, n_steps=52, seed=1)
    eng_pricing = engine.PricingContext(pricing.to_params())
    assert eng_pricing.n_paths == 100
    assert eng_pricing.n_steps == 52


def test_execution_context_defaults_to_auto():
    execution = q.ExecutionContext()
    assert execution.backend == "auto"
    eng_execution = engine.ExecutionContext(execution.to_params())
    expected = "gpu" if engine.is_gpu_backend_available() else "cpu"
    assert eng_execution.backend == expected


def test_execution_context_rejects_unknown_backend():
    try:
        q.ExecutionContext(backend="quantum")
        assert False, "se esperaba ValidationError"
    except pydantic.ValidationError:
        pass


if __name__ == "__main__":
    test_hull_white_1f_to_params_feeds_the_real_engine()
    test_hull_white_2f_to_params_feeds_the_real_engine()
    test_hull_white_requires_all_fields()
    test_market_rejects_non_increasing_pillars()
    test_market_rejects_mismatched_lengths()
    test_market_to_params_feeds_the_real_engine()
    test_pricing_context_rejects_non_positive_paths()
    test_pricing_context_to_params_feeds_the_real_engine()
    test_execution_context_defaults_to_auto()
    test_execution_context_rejects_unknown_backend()
    print("OK: tests de engine_typed.Model/Market/PricingContext/ExecutionContext pasaron")
