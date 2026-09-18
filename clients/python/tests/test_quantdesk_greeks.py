"""Tests de `quantdesk.greeks`/`Engine.all_greeks` (PLAN_GREEKS.md §8.1/§8.4/§8.5, Fase 9):
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

import pytest  # noqa: E402

import engine  # noqa: E402
import quantdesk as q  # noqa: E402
from quantdesk import greeks  # noqa: E402


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


def test_cross_gamma_sets_the_cross_factor_with_the_model_prefix():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 4: "cross_factor" es nuevo en el bag de Params de "Greek" --
    # antes de esta fase GreekMeasure nunca lo leia (siempre nullopt del lado C++).
    spec = greeks.cross_gamma("PayoffPriceQ", "spot_0", "spot_1").to_spec()
    assert spec[1]["risk_factor"] == "model.spot_0"
    assert spec[1]["cross_factor"] == "model.spot_1"
    assert spec[1]["order"] == 1.0


def test_delta_without_cross_gamma_never_sends_a_cross_factor_key():
    # Ausencia = comportamiento identico al de antes de esta fase (Greek.cross_factor por
    # defecto es None, to_params() lo omite del bag en vez de mandar None).
    spec = greeks.delta("PayoffPriceQ", "spot").to_spec()
    assert "cross_factor" not in spec[1]


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


def test_theta_measure_result_exposes_bump_used_matching_the_effective_bump():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 5: engine.MeasureResult.bump_used (nuevo) para una "Greek"
    # con bump=None debe coincidir EXACTAMENTE con el bump que el motor resolvio internamente
    # (default_time_shift_bump() en cpp/engine/src/greeks.cpp) -- verificado repitiendo la
    # llamada con ESE bump explicito y comprobando que el escalar no cambia.
    eng, product, model, market, pricing, execution = _gbm_call_fixture()
    greek = greeks.theta("PayoffPriceQ")  # bump=None
    result = eng.price(product, [greek.to_spec()], model, market, pricing, execution)["Greek"]
    assert result.bump_used is not None

    explicit_greek = greeks.theta("PayoffPriceQ", bump=result.bump_used)
    explicit_result = eng.price(product, [explicit_greek.to_spec()], model, market, pricing, execution)["Greek"]
    assert math.isclose(explicit_result.scalar, result.scalar, rel_tol=0.0, abs_tol=1e-9)

    # Medidas que no son "Greek" nunca rellenan bump_used (no aplica, PLAN_IMPROVE_NOTEBOOK2.md
    # Fase 5) -- se deja explicitamente None, nunca un valor inventado.
    pv_result = eng.price(product, ["PayoffPriceQ"], model, market, pricing, execution)["PayoffPriceQ"]
    assert pv_result.bump_used is None


def test_theta_annualized_divides_the_raw_bump_delta_by_measure_result_bump_used():
    # PLAN_IMPROVE_NOTEBOOK.md Fase 4 (ADR-IN-01): annualized=False (default) devuelve el
    # DeltaV crudo del bump (convencion historica, sin cambios); annualized=True devuelve
    # DeltaV/bump (derivada anualizada dV/dt). PLAN_IMPROVE_NOTEBOOK2.md Fase 5:
    # ThetaGreek.annualize() ahora recibe el MeasureResult completo (no solo el escalar) y lee
    # `bump_used` de ahi -- ya no depende de la constante Python DEFAULT_THETA_BUMP (retirada).
    eng, product, model, market, pricing, execution = _gbm_call_fixture()

    raw_greek = greeks.theta("PayoffPriceQ")
    raw_result = eng.price(product, [raw_greek.to_spec()], model, market, pricing, execution)["Greek"]
    assert raw_greek.annualize(raw_result) == raw_result.scalar  # annualized=False: paso-through

    annualized_greek = greeks.theta("PayoffPriceQ", annualized=True)
    spec = annualized_greek.to_spec()
    assert "bump" not in spec[1]  # bump=None se deja pasar tal cual, el motor resuelve su default
    annualized_result = eng.price(product, [spec], model, market, pricing, execution)["Greek"]
    annualized_theta = annualized_greek.annualize(annualized_result)

    # Mismo bump por defecto en ambas llamadas (bump=None en las dos) => ambos escalares crudos
    # deben coincidir (mismo bump-and-reval), y el anualizado debe ser exactamente ese crudo
    # dividido por el bump_used que reporto el motor.
    assert math.isclose(annualized_result.scalar, raw_result.scalar, rel_tol=0.0, abs_tol=1e-9)
    assert annualized_result.bump_used is not None
    assert math.isclose(
        annualized_theta, raw_result.scalar / annualized_result.bump_used, rel_tol=0.0, abs_tol=1e-9
    )


def test_payoff_sensitivity_q_is_a_pathwise_alias_of_greek():
    # PLAN_IMPROVE_NOTEBOOK.md Fase 1: decision de diseno -- para las 4 sensibilidades que cubre
    # PayoffSensitivityQMeasure ("spot"/"rate"/"dividend_yield"/"volatility", solo GBM),
    # `PayoffSensitivityQ` y `Greek(metric="PayoffPriceQ", method="pathwise")` llaman
    # literalmente a la misma funcion Rust `payoff_sensitivity_gbm` (ver `try_pathwise` en
    # cpp/engine/src/greeks.cpp): mismo model/product/n_paths/seed en las dos rutas produce el
    # mismo resultado hasta precision numerica, no solo dentro de ruido Monte Carlo -- de ahi la
    # tolerancia mucho mas ajustada que el resto de este archivo (que compara contra
    # Black-Scholes, dos fuentes genuinamente distintas). Mismo par de rutas y mismas 4
    # sensibilidades que GreeksFase1Test.*MatchesPayoffSensitivityQ en test_greeks.cpp.
    eng, product, model, market, pricing, execution = _gbm_call_fixture()

    for greek_name, spec in (
        ("spot", greeks.delta("PayoffPriceQ", "spot").to_spec()),
        ("volatility", greeks.vega("PayoffPriceQ").to_spec()),
        ("rate", greeks.rho("PayoffPriceQ").to_spec()),
        ("dividend_yield", greeks.delta("PayoffPriceQ", "dividend_yield").to_spec()),
    ):
        via_greek = eng.price(product, [spec], model, market, pricing, execution)["Greek"].scalar
        via_registry = eng.price(
            product, [("PayoffSensitivityQ", {"greek": greek_name})], model, market, pricing, execution
        )["PayoffSensitivityQ"].scalar
        assert math.isclose(via_greek, via_registry, rel_tol=1e-6, abs_tol=1e-6), (
            f"{greek_name}: Greek(pathwise)={via_greek} PayoffSensitivityQ={via_registry}"
        )


def test_payoff_sensitivity_q_rejects_a_product_that_is_not_a_payoff_product():
    # Mismo chequeo defensivo que el test C++
    # PayoffSensitivityQRejectsAProductThatIsNotPayoffProduct (test_registry_wiring_payoff_
    # measures.cpp) -- confirma que el binding Python propaga el std::invalid_argument como
    # excepcion, no como un resultado silenciosamente vacio.
    eng = engine.Engine()
    model = eng.create_model("GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"})
    irs_product = eng.create_product(
        "IRSwap",
        {
            "notional": 1_000_000.0,
            "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
            "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
        },
    )
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.05])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 1000.0, "n_steps": 1.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu"})
    try:
        eng.price(irs_product, [("PayoffSensitivityQ", {"greek": "spot"})], model, market, pricing, execution)
    except Exception:
        return
    raise AssertionError("se esperaba una excepcion: PayoffSensitivityQ sobre un producto que no es PayoffProduct")


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


def test_hessian_on_a_calendar_spread_reports_the_multi_date_reason_in_skipped():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 2 (paridad Python del test C++
    # GreeksHessianTest.ComputeHessianOnACalendarSpreadGoesToSkippedWithTheMultiDateReasonNotThe
    # GenericOne en test_greeks.cpp): un calendar spread (dos patas del mismo observable en
    # fechas distintas, mismo patron que long_calendar_spread en
    # 09_option_strategies_and_greeks.ipynb) no depende de una unica fecha terminal -- gamma/
    # vanna/volga no se calculan (sin fallback numerico, decision (b) de esta fase), pero
    # `HessianReport.skipped` debe nombrar el motivo EXACTO (multi-fecha), no el mensaje generico
    # de "combinacion no cubierta" (esa combinacion (GBM, PayoffPriceQ) SI esta cubierta para un
    # contrato de una unica fecha, ver test_hessian_on_gbm_call_returns_gamma_volga_vanna_via_
    # likelihood_ratio de arriba).
    eng = engine.Engine()
    legs = [
        q.call_leg("EQ.SPOT.AAPL", 100.0, -1.0, 0.25),
        q.call_leg("EQ.SPOT.AAPL", 100.0, 1.0, 1.0),
    ]
    trade = q.custom_strategy("CALENDAR_SPREAD", legs)
    product = eng.create_product(trade.product_type, trade.to_params())
    model = eng.create_model("GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"})
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.05])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 20_000.0, "n_steps": 1.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu"})

    report = eng.hessian(product, "PayoffPriceQ", model, market, pricing, execution)

    assert report.entries == []
    assert len(report.skipped) == 1
    reason = report.skipped[0]
    assert "unica fecha" in reason
    assert "no esta cubierta por hessian_capabilities()" not in reason


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


def _gbm_basket_call_fixture(s0=(100.0, 100.0), sigma=(0.2, 0.2), rho=0.4, strike=190.0, n_paths=300_000, seed=11):
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 4: fixture de basket de 2 activos con delta/cross-gamma
    # alcanzables via greeks.delta/cross_gamma -- mismo patron que _gbm_call_fixture arriba, pero
    # GbmBasket en vez de GBM (n_assets=2) y un contrato de basket call sobre la SUMA.
    eng = engine.Engine()
    model = eng.create_model(
        "GbmBasket",
        {
            "observables": ["EQ.SPOT.A", "EQ.SPOT.B"],
            "s0": list(s0),
            "r": [0.03, 0.03],
            "q": [0.0, 0.0],
            "sigma": list(sigma),
            "correlation": [1.0, rho, rho, 1.0],
        },
    )
    total = q.fixing("EQ.SPOT.A", 1.0) + q.fixing("EQ.SPOT.B", 1.0)
    contract = q.when(1.0, q.cashflow("USD", q.maximum(total - strike, 0.0)))
    product = eng.create_product("Payoff", q.PayoffProduct(id="BASKET_CALL", contract=contract).to_params())
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.03])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": float(n_paths), "n_steps": 1.0, "seed": float(seed)})
    execution = engine.ExecutionContext({"backend": "cpu"})
    return eng, product, model, market, pricing, execution


def _basket_delta_via_manual_bump(eng, product, s0, sigma, rho, strike, asset, h, market, pricing, execution):
    """Oraculo manual: reconstruye GbmBasketModel con solo `asset` desplazado en +-h (mismos
    numeros aleatorios comunes, mismo seed/n_paths) y calcula la diferencia central -- exactamente
    lo que hace bump_state/compute_greek por dentro (ver test_greeks.cpp,
    GreeksImproveNotebook2Fase4Test, para el equivalente C++ de este mismo oraculo)."""

    def _model_with_bump(delta):
        bumped_s0 = list(s0)
        bumped_s0[asset] += delta
        return eng.create_model(
            "GbmBasket",
            {
                "observables": ["EQ.SPOT.A", "EQ.SPOT.B"],
                "s0": bumped_s0,
                "r": [0.03, 0.03],
                "q": [0.0, 0.0],
                "sigma": list(sigma),
                "correlation": [1.0, rho, rho, 1.0],
            },
        )

    up = eng.price(product, ["PayoffPriceQ"], _model_with_bump(h), market, pricing, execution)["PayoffPriceQ"].scalar
    down = eng.price(product, ["PayoffPriceQ"], _model_with_bump(-h), market, pricing, execution)["PayoffPriceQ"].scalar
    return (up - down) / (2.0 * h)


def test_delta_per_asset_of_a_basket_call_matches_manual_bump_and_reval():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 4, criterio de aceptacion explicito: "un basket de 2 activos
    # tiene delta por activo alcanzable via greeks.delta(...), verificado contra bump-and-reval
    # manual". method="auto" cae SIEMPRE a bump-and-reval para GbmBasket (no esta en
    # pathwise_capabilities() del lado C++) -- se confirma leyendo method_used si estuviera
    # expuesto (no lo esta en MeasureResult, solo en GreekResult/all_greeks), asi que aqui basta
    # con la paridad numerica contra el oraculo manual.
    s0, sigma, rho, strike = (100.0, 100.0), (0.2, 0.2), 0.4, 190.0
    eng, product, model, market, pricing, execution = _gbm_basket_call_fixture(s0=s0, sigma=sigma, rho=rho, strike=strike)

    for asset, risk_factor in ((0, "spot_0"), (1, "spot_1")):
        result = eng.price(product, [greeks.delta("PayoffPriceQ", risk_factor).to_spec()], model, market, pricing, execution)
        delta = result["Greek"].scalar
        bump_used = result["Greek"].bump_used
        assert bump_used is not None
        manual_delta = _basket_delta_via_manual_bump(
            eng, product, s0, sigma, rho, strike, asset, bump_used, market, pricing, execution
        )
        assert math.isclose(delta, manual_delta, rel_tol=0.0, abs_tol=1e-9), (
            f"asset={asset} delta={delta} manual={manual_delta}"
        )


def test_delta_per_asset_rejects_an_out_of_range_asset_index_explicitly():
    eng, product, model, market, pricing, execution = _gbm_basket_call_fixture()
    with pytest.raises(Exception):
        eng.price(product, [greeks.delta("PayoffPriceQ", "spot_7").to_spec()], model, market, pricing, execution)


def test_cross_gamma_between_two_assets_of_a_basket_matches_manual_four_point_stencil():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 4: la cross-gamma real entre dos activos (d^2V/dS_0 dS_1),
    # alcanzable via greeks.cross_gamma(...) sobre Engine.price(...) (GreekMeasure ahora parsea
    # "cross_factor") -- verificada contra el mismo estencil de 4 puntos calculado a mano.
    s0, sigma, rho, strike = (100.0, 100.0), (0.2, 0.2), 0.4, 190.0
    eng, product, model, market, pricing, execution = _gbm_basket_call_fixture(s0=s0, sigma=sigma, rho=rho, strike=strike)

    spec = greeks.cross_gamma("PayoffPriceQ", "spot_0", "spot_1").to_spec()
    assert spec[1]["cross_factor"] == "model.spot_1"
    result = eng.price(product, [spec], model, market, pricing, execution)["Greek"]
    cross_gamma_value = result.scalar
    h1 = result.bump_used
    assert h1 is not None
    h2 = max(1e-2 * s0[1], 1e-4)  # cross_factor siempre usa su propio default (ver resolve_bump)

    def _model_with_bumps(d0, d1):
        bumped_s0 = [s0[0] + d0, s0[1] + d1]
        return eng.create_model(
            "GbmBasket",
            {
                "observables": ["EQ.SPOT.A", "EQ.SPOT.B"],
                "s0": bumped_s0,
                "r": [0.03, 0.03],
                "q": [0.0, 0.0],
                "sigma": list(sigma),
                "correlation": [1.0, rho, rho, 1.0],
            },
        )

    def _price(d0, d1):
        m = _model_with_bumps(d0, d1)
        return eng.price(product, ["PayoffPriceQ"], m, market, pricing, execution)["PayoffPriceQ"].scalar

    manual = (_price(h1, h2) - _price(h1, -h2) - _price(-h1, h2) + _price(-h1, -h2)) / (4.0 * h1 * h2)
    assert math.isclose(cross_gamma_value, manual, rel_tol=0.0, abs_tol=1e-9), (
        f"cross_gamma={cross_gamma_value} manual={manual}"
    )


if __name__ == "__main__":
    test_delta_to_spec_uses_the_model_prefix()
    test_vega_and_rho_default_risk_factors()
    test_gamma_sets_order_two_on_the_same_risk_factor()
    test_cross_gamma_sets_the_cross_factor_with_the_model_prefix()
    test_delta_without_cross_gamma_never_sends_a_cross_factor_key()
    test_dv01_defaults_to_pv_and_curve_parallel()
    test_dv01_with_pillar_uses_curve_pillar()
    test_theta_and_credit_builders_default_metrics()
    test_greek_forwards_metric_params_with_the_metric_dot_prefix()
    test_greek_forwards_an_explicit_bump_and_method()
    test_builders_forward_an_explicit_bump_and_method_too()
    test_delta_of_a_call_through_price_matches_black_scholes()
    test_theta_measure_result_exposes_bump_used_matching_the_effective_bump()
    test_theta_annualized_divides_the_raw_bump_delta_by_measure_result_bump_used()
    test_payoff_sensitivity_q_is_a_pathwise_alias_of_greek()
    test_payoff_sensitivity_q_rejects_a_product_that_is_not_a_payoff_product()
    test_all_greeks_on_gbm_returns_the_four_model_parameters_plus_curve_credit_and_theta()
    test_all_greeks_include_second_order_adds_gamma_for_each_model_parameter()
    test_hessian_on_gbm_call_returns_gamma_volga_vanna_via_likelihood_ratio()
    test_hvp_on_gbm_call_with_unit_direction_matches_the_hessian_row()
    test_hessian_on_hull_white1f_swap_returns_ten_entries_via_forward_over_forward()
    test_hessian_with_explicit_factors_restricts_to_the_requested_sub_hessian()
    test_hvp_on_hull_white1f_swap_with_unit_direction_on_a_matches_the_hessian_row()
    test_hvp_with_empty_direction_raises()
    test_delta_per_asset_of_a_basket_call_matches_manual_bump_and_reval()
    test_delta_per_asset_rejects_an_out_of_range_asset_index_explicitly()
    test_cross_gamma_between_two_assets_of_a_basket_matches_manual_four_point_stencil()
    print("OK: tests de quantdesk.greeks/Engine.all_greeks/Engine.hessian/Engine.hvp pasaron")
