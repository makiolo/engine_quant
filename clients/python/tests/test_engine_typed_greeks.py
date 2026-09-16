"""Tests de `engine_typed.greeks`/`Engine.all_greeks` (PLAN_GREEKS.md §8.1/§8.4/§8.5, Fase 9):
`Greek.to_spec()` produce el mismo `(nombre, params)` que consume `Engine.price` (equivalente
Python de `GreekMeasure`/`compute_greek` en test_greeks.cpp), y `Engine.all_greeks` expone el
barrido automatico (`compute_all_greeks`) sin que el cliente Python enumere cada factor de
riesgo a mano. No repite la verificacion numerica fina de las Greeks (ya cubierta
exhaustivamente por `GreeksFaseNTest` en test_greeks.cpp) -- aqui se confirma que el binding
nanobind expone la misma superficie/semantica que la capa C++.
"""

import math
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402
import engine_typed as q  # noqa: E402
from engine_typed import greeks  # noqa: E402


def test_delta_to_spec_uses_the_model_prefix():
    assert greeks.delta("PayoffPriceQ", "spot").to_spec() == (
        "Greek", {"metric": "PayoffPriceQ", "risk_factor": "model.spot", "order": 1.0, "method": "auto"}
    )


def test_vega_and_rho_default_risk_factors():
    assert greeks.vega("PayoffPriceQ").to_spec()[1]["risk_factor"] == "model.volatility"
    assert greeks.rho("PayoffPriceQ").to_spec()[1]["risk_factor"] == "model.rate"
    assert greeks.rho("HullWhiteModelNpv", risk_factor="r0").to_spec()[1]["risk_factor"] == "model.r0"


def test_gamma_sets_order_two_on_the_same_risk_factor():
    spec = greeks.gamma("PayoffPriceQ", "spot").to_spec()
    assert spec[1]["risk_factor"] == "model.spot"
    assert spec[1]["order"] == 2.0


def test_dv01_defaults_to_pv_and_curve_parallel():
    assert greeks.dv01().to_spec() == ("Greek", {"metric": "PV", "risk_factor": "curve.parallel", "order": 1.0, "method": "auto"})


def test_dv01_with_pillar_uses_curve_pillar():
    assert greeks.dv01(pillar=2).to_spec()[1]["risk_factor"] == "curve.pillar:2"


def test_theta_and_credit_builders_default_metrics():
    assert greeks.theta().to_spec()[1] == {"metric": "PV", "risk_factor": "time.theta", "order": 1.0, "method": "auto"}
    assert greeks.hazard_rate().to_spec()[1]["metric"] == "UnilateralCVA"
    assert greeks.hazard_rate().to_spec()[1]["risk_factor"] == "credit.hazard_rate"
    assert greeks.recovery_rate().to_spec()[1]["risk_factor"] == "credit.recovery_rate"


def test_greek_forwards_metric_params_with_the_metric_dot_prefix():
    spec = greeks.delta("PayoffHitProbabilityQ", "spot", event="UI").to_spec()
    assert spec[1]["metric.event"] == "UI"


def test_greek_forwards_an_explicit_bump_and_method():
    spec = q.Greek(metric="PV", risk_factor="curve.parallel", bump=0.0002, method="bump_and_reval").to_spec()
    assert spec[1]["bump"] == 0.0002
    assert spec[1]["method"] == "bump_and_reval"


def test_builders_forward_an_explicit_bump_and_method_too():
    assert greeks.dv01(bump=0.0002).to_spec()[1]["bump"] == 0.0002
    assert greeks.theta("PayoffPriceQ", bump=2.0).to_spec()[1]["bump"] == 2.0
    assert greeks.delta("PayoffPriceQ", "spot", method="pathwise").to_spec()[1]["method"] == "pathwise"


def _gbm_call_fixture():
    eng = engine.Engine()
    trade = q.european_call("AAPL_CALL_100", "EQ.SPOT.AAPL", strike=100.0, notional=1_000.0, maturity=1.0)
    product = eng.create_product(trade.product_type, trade.to_params())
    model = eng.create_model("GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"})
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.05])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 200_000.0, "n_steps": 1.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu"})
    return eng, product, model, market, pricing, execution


def _black_scholes_call_delta(s0, k, r, q_div, sigma, t):
    d1 = (math.log(s0 / k) + (r - q_div + 0.5 * sigma * sigma) * t) / (sigma * math.sqrt(t))
    return math.exp(-q_div * t) * 0.5 * (1.0 + math.erf(d1 / math.sqrt(2.0)))


def test_delta_of_a_call_through_price_matches_black_scholes():
    eng, product, model, market, pricing, execution = _gbm_call_fixture()
    result = eng.price(product, [greeks.delta("PayoffPriceQ", "spot").to_spec()], model, market, pricing, execution)
    delta = result["Greek"].scalar
    expected = _black_scholes_call_delta(100.0, 100.0, 0.05, 0.0, 0.2, 1.0) * 1_000.0
    assert math.isclose(delta, expected, rel_tol=0.05)


def test_all_greeks_on_gbm_returns_the_four_model_parameters_plus_curve_credit_and_theta():
    eng, product, model, market, pricing, execution = _gbm_call_fixture()
    report = eng.all_greeks(product, "PayoffPriceQ", model, market, pricing, execution)

    risk_factors = {g.risk_factor for g in report.greeks}
    assert risk_factors == {
        "model.spot", "model.rate", "model.dividend_yield", "model.volatility",
        "curve.parallel", "credit.hazard_rate", "credit.recovery_rate", "time.theta",
    }
    assert report.skipped == []

    by_factor = {g.risk_factor: g for g in report.greeks}
    assert by_factor["model.spot"].measure == "RiskNeutralQ"
    assert by_factor["model.spot"].method_used in ("pathwise", "bump_and_reval")
    # curva/credito: PayoffPriceQ de un GBM sobre un unico observable no consume ninguno de los
    # dos -- derivada nula (PLAN_GREEKS.md §8.5: "una derivada nula es una respuesta valida,
    # distinta de 'no aplica'"), pero el bump SI se aplico (bump_used presente).
    assert by_factor["credit.hazard_rate"].value == 0.0
    assert by_factor["credit.hazard_rate"].bump_used is not None


def test_all_greeks_include_second_order_adds_gamma_for_each_model_parameter():
    eng, product, model, market, pricing, execution = _gbm_call_fixture()
    report = eng.all_greeks(
        product, "PayoffPriceQ", model, market, pricing, execution, include_second_order=True
    )
    orders = {(g.risk_factor, g.order) for g in report.greeks}
    assert ("model.spot", 1) in orders
    assert ("model.spot", 2) in orders
    # Gamma pura nunca se enumera para curva/credito/tiempo (PLAN_GREEKS.md §8.5 punto 5).
    assert ("curve.parallel", 2) not in orders
    assert ("time.theta", 2) not in orders


# --- Engine.hessian/Engine.hvp (PLAN_BACKWARD.md §8.1/§9 Fase 1-3): mismo criterio de arriba --
# aqui solo se confirma que el binding nanobind expone la misma superficie/semantica que
# engine::greeks::compute_hessian/compute_hvp (ya verificados exhaustivamente por
# GreeksHessianTest/GreeksHvpTest en cpp/engine/tests/test_greeks.cpp), no se repite la
# verificacion numerica fina.


def _hull_white_params():
    return {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02}


def _par_irs_5y_params():
    return {
        "notional": 1_000_000.0,
        "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
        "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
    }


def _hull_white_swap_fixture():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    product = eng.create_product("IRSwap", _par_irs_5y_params())
    market = engine.MarketSnapshot(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 1000.0, "n_steps": 1.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu"})
    return eng, product, model, market, pricing, execution


def test_hessian_on_gbm_call_returns_gamma_volga_vanna_via_likelihood_ratio():
    eng, product, model, market, pricing, execution = _gbm_call_fixture()
    report = eng.hessian(product, "PayoffPriceQ", model, market, pricing, execution)

    assert report.skipped == []
    pairs = {(e.factor_i, e.factor_j) for e in report.entries}
    assert pairs == {
        ("model.spot", "model.spot"),
        ("model.volatility", "model.volatility"),
        ("model.spot", "model.volatility"),
    }
    for e in report.entries:
        assert e.method_used == "likelihood_ratio_hessian"
        assert e.measure == "RiskNeutralQ"
        assert e.std_error is not None


def test_hvp_on_gbm_call_with_unit_direction_matches_the_hessian_row():
    eng, product, model, market, pricing, execution = _gbm_call_fixture()
    hessian_report = eng.hessian(product, "PayoffPriceQ", model, market, pricing, execution)
    hvp_report = eng.hvp(
        product, "PayoffPriceQ", model, market, pricing, execution,
        direction={"model.spot": 1.0, "model.volatility": 0.0},
    )

    assert hvp_report.skipped == []
    by_pair = {(e.factor_i, e.factor_j): e.value for e in hessian_report.entries}
    hessian_row_spot = {
        "model.spot": by_pair[("model.spot", "model.spot")],
        "model.volatility": by_pair[("model.spot", "model.volatility")],
    }
    assert len(hvp_report.components) == 2
    for c in hvp_report.components:
        assert math.isclose(c.value, hessian_row_spot[c.factor], rel_tol=0.0, abs_tol=1e-9)


def test_hessian_on_hull_white1f_swap_returns_ten_entries_via_forward_over_forward():
    eng, product, model, market, pricing, execution = _hull_white_swap_fixture()
    report = eng.hessian(product, "HullWhiteModelNpv", model, market, pricing, execution)

    assert report.skipped == []
    assert len(report.entries) == 10
    for e in report.entries:
        assert e.method_used == "aad_forward_over_forward"


def test_hessian_with_explicit_factors_restricts_to_the_requested_sub_hessian():
    eng, product, model, market, pricing, execution = _hull_white_swap_fixture()
    report = eng.hessian(product, "HullWhiteModelNpv", model, market, pricing, execution, risk_factors=["model.a"])

    assert len(report.entries) == 1
    assert report.entries[0].factor_i == "model.a"
    assert report.entries[0].factor_j == "model.a"


def test_hvp_on_hull_white1f_swap_with_unit_direction_on_a_matches_the_hessian_row():
    eng, product, model, market, pricing, execution = _hull_white_swap_fixture()
    hessian_report = eng.hessian(product, "HullWhiteModelNpv", model, market, pricing, execution)
    hvp_report = eng.hvp(
        product, "HullWhiteModelNpv", model, market, pricing, execution,
        direction={"model.a": 1.0, "model.b": 0.0, "model.sigma": 0.0, "model.r0": 0.0},
    )

    assert hvp_report.skipped == []
    hessian_row_a = {}
    for e in hessian_report.entries:
        if e.factor_i == "model.a":
            hessian_row_a[e.factor_j] = e.value
        elif e.factor_j == "model.a":
            hessian_row_a[e.factor_i] = e.value

    assert len(hvp_report.components) == 4
    for c in hvp_report.components:
        assert math.isclose(c.value, hessian_row_a[c.factor], rel_tol=0.0, abs_tol=1e-8)


def test_hvp_with_empty_direction_raises():
    eng, product, model, market, pricing, execution = _hull_white_swap_fixture()
    try:
        eng.hvp(product, "HullWhiteModelNpv", model, market, pricing, execution, direction={})
    except Exception:
        return
    raise AssertionError("se esperaba una excepcion con direction={} (HVP sin direccion no significa nada)")


if __name__ == "__main__":
    test_delta_to_spec_uses_the_model_prefix()
    test_vega_and_rho_default_risk_factors()
    test_gamma_sets_order_two_on_the_same_risk_factor()
    test_dv01_defaults_to_pv_and_curve_parallel()
    test_dv01_with_pillar_uses_curve_pillar()
    test_theta_and_credit_builders_default_metrics()
    test_greek_forwards_metric_params_with_the_metric_dot_prefix()
    test_greek_forwards_an_explicit_bump_and_method()
    test_builders_forward_an_explicit_bump_and_method_too()
    test_delta_of_a_call_through_price_matches_black_scholes()
    test_all_greeks_on_gbm_returns_the_four_model_parameters_plus_curve_credit_and_theta()
    test_all_greeks_include_second_order_adds_gamma_for_each_model_parameter()
    test_hessian_on_gbm_call_returns_gamma_volga_vanna_via_likelihood_ratio()
    test_hvp_on_gbm_call_with_unit_direction_matches_the_hessian_row()
    test_hessian_on_hull_white1f_swap_returns_ten_entries_via_forward_over_forward()
    test_hessian_with_explicit_factors_restricts_to_the_requested_sub_hessian()
    test_hvp_on_hull_white1f_swap_with_unit_direction_on_a_matches_the_hessian_row()
    test_hvp_with_empty_direction_raises()
    print("OK: tests de engine_typed.greeks/Engine.all_greeks/Engine.hessian/Engine.hvp pasaron")
