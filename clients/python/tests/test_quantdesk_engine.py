"""Tests de `quantdesk.Engine`/`PriceResult`/`BatchRow`/`GridRow` (PLAN_API_REFACTOR.md Fase 1-2,
§3.2/§3.3) -- sin cobertura dedicada hasta esta fase (Fase 6, "Estado verificado" de Fase 1/2 ya
documentó los mismos números a mano en un script ad-hoc de sesión, no en un test versionado).

Criterio explicito de esta fase (PLAN_API_REFACTOR.md Fase 6):
  - `Engine.price(...)` da el mismo resultado que el flujo nativo equivalente (comparación bit a
    bit, no con tolerancia -- mismo criterio que el resto de este repositorio).
  - `results.PV.scalar == results["PV"].scalar`, y además el MISMO objeto (`is`), no solo valores
    iguales -- `PriceResult` no reimplementa nada, solo envuelve el dict nativo (§3.3).
  - Un override puntual de `pricing=`/`execution=` en una llamada NO muta el `Engine` para
    llamadas siguientes -- verificado con una medida Monte Carlo real (`ExpectedExposure`, que
    depende de `n_paths`/`seed`), no con `PV`/`DV01` de un IRS vainilla (deterministas: el test
    pasaría igual aunque `Engine` mutara realmente, PLAN_API_REFACTOR.md Fase 1, nota para Fase 6).
  - Cobertura mínima de `price_batch`/`price_many`/`price_grid`/`all_greeks`/`hessian`/`hvp`/
    `simulate_paths`/`calibrate`/`list_*`/`Portfolio`, comparando siempre contra el flujo nativo
    equivalente (mismo criterio que ya aplican `test_quantdesk_greeks.py`/`test_portfolio.py`).
  - Cobertura de `Gbm`/`GbmP` (`quantdesk.model`) y `Engine.evaluate_scenario`, piezas nuevas de
    PLAN_API_REFACTOR.md Fase 5 sin test dedicado hasta ahora (nota explícita de Fase 5 para
    Fase 6, ver PLAN_API_REFACTOR.md).
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import pytest  # noqa: E402

import engine  # noqa: E402
import quantdesk as q  # noqa: E402
from quantdesk import Engine  # noqa: E402


# =============================================================================================
# Fixtures compartidas -- mismo caso que el bloque "Después" de PLAN_API_REFACTOR.md §1, ya
# verificado a mano en la sesión de Fase 1 (PV=948.4537547220389, DV01=480.18800212936185,
# UnilateralCVA=626.7254432766481) -- aquí se recalcula dinámicamente contra el flujo nativo en
# vez de hardcodear esos valores como "golden data", mismo criterio que el resto del repo.
# =============================================================================================


def _irs_hull_white_case():
    trade = q.IRSwap(
        notional=1_000_000.0, fixed_rate=0.02,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    model = q.HullWhite1F(a=0.10, b=0.03, sigma=0.01, r0=0.02)
    market = q.Market(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02], hazard_rate=0.02, recovery_rate=0.40)
    return trade, model, market


_METRICS = ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"]


def _native_pricing(n_paths, n_steps, seed, pricing_date=0.0):
    return engine.PricingContext(
        {"pricing_date": pricing_date, "n_paths": float(n_paths), "n_steps": float(n_steps), "seed": float(seed)}
    )


def _native_execution(backend="cpu", precision="FP64"):
    return engine.ExecutionContext({"backend": backend, "precision": precision})


def _native_metrics(metrics):
    return [m.to_spec() if isinstance(m, q.Measure) else m for m in metrics]


def _native_price(trade, model, market, metrics, n_paths, n_steps, seed, backend="cpu"):
    eng = engine.Engine()
    native_product = eng.create_product(trade.product_type, trade.to_params())
    native_model = eng.create_model(model.model_type, model.to_params())
    native_market = engine.MarketSnapshot(**market.to_params())
    return eng.price(
        native_product, _native_metrics(metrics), native_model, native_market,
        _native_pricing(n_paths, n_steps, seed), _native_execution(backend),
    )


def _assert_measure_result_equal(a, b):
    assert a.has_scalar == b.has_scalar
    if a.has_scalar:
        assert a.scalar == b.scalar
    assert list(a.times) == list(b.times)
    assert list(a.primary) == list(b.primary)
    assert list(a.secondary) == list(b.secondary)
    assert a.bump_used == b.bump_used


# =============================================================================================
# Engine.price -- paridad bit a bit contra el flujo nativo, PriceResult.__getattr__, no-mutación
# de overrides puntuales de pricing=/execution=.
# =============================================================================================


def test_price_matches_the_native_flow_bit_for_bit():
    trade, model, market = _irs_hull_white_case()
    qeng = Engine(backend="cpu", n_paths=5_000, n_steps=208, seed=7)

    results = qeng.price(trade, model, market, _METRICS)
    native_results = _native_price(trade, model, market, _METRICS, n_paths=5_000, n_steps=208, seed=7)

    assert set(results.keys()) == set(native_results.keys()) == set(_METRICS)
    for name in _METRICS:
        _assert_measure_result_equal(results[name], native_results[name])


def test_price_result_dot_access_matches_bracket_access_and_is_the_same_object():
    trade, model, market = _irs_hull_white_case()
    qeng = Engine(backend="cpu", n_paths=1, n_steps=208, seed=1)  # PV/DV01: deterministas
    results = qeng.price(trade, model, market, ["PV", "DV01", "UnilateralCVA"])

    for name in ("PV", "DV01", "UnilateralCVA"):
        assert getattr(results, name).scalar == results[name].scalar
        # No solo valores iguales -- el MISMO objeto engine.MeasureResult (§3.3: PriceResult no
        # reimplementa nada, solo envuelve el dict nativo).
        assert getattr(results, name) is results[name]


def test_price_result_dot_access_raises_attribute_error_for_a_measure_not_requested():
    trade, model, market = _irs_hull_white_case()
    qeng = Engine(backend="cpu", n_paths=1, n_steps=208, seed=1)
    results = qeng.price(trade, model, market, ["PV"])
    with pytest.raises(AttributeError):
        results.DV01  # noqa: B018 -- el acceso en sí es la aserción


def test_price_override_pricing_has_a_real_effect_but_does_not_mutate_the_engine():
    # ExpectedExposure/PFE95 dependen de la simulación Monte Carlo de Hull-White (n_paths/seed) --
    # a diferencia de PV/DV01 de un IRS vainilla (bump-and-reval determinista), un override que
    # NO tuviera efecto real seguiría pasando el test si se usara PV/DV01 (PLAN_API_REFACTOR.md
    # Fase 1, nota explícita para Fase 6). Aquí sí se puede distinguir "el override funcionó" de
    # "el override mutó el Engine para llamadas siguientes".
    trade, model, market = _irs_hull_white_case()
    qeng = Engine(backend="cpu", n_paths=5_000, n_steps=208, seed=7)

    baseline = qeng.price(trade, model, market, ["ExpectedExposure"])
    baseline_profile = list(baseline["ExpectedExposure"].primary)

    override_pricing = q.PricingContext(n_paths=200, n_steps=208, seed=123)
    overridden = qeng.price(trade, model, market, ["ExpectedExposure"], pricing=override_pricing)
    overridden_profile = list(overridden["ExpectedExposure"].primary)

    # El override tuvo un efecto real en ESTA llamada (distinto n_paths/seed -> perfil distinto).
    assert overridden_profile != baseline_profile

    # El Engine no quedó mutado: los atributos fijados en el constructor siguen intactos...
    assert qeng._pricing == q.PricingContext(n_paths=5_000, n_steps=208, seed=7)

    # ...y una llamada SIN override posterior reproduce el baseline EXACTO, no un valor nuevo.
    after = qeng.price(trade, model, market, ["ExpectedExposure"])
    assert list(after["ExpectedExposure"].primary) == baseline_profile


def test_price_override_execution_does_not_mutate_the_engine_for_subsequent_calls():
    trade, model, market = _irs_hull_white_case()
    qeng = Engine(backend="auto", n_paths=1_000, n_steps=208, seed=7)
    assert qeng._execution.backend == "auto"

    qeng.price(trade, model, market, ["PV"], execution=q.ExecutionContext(backend="cpu"))

    # El override puntual no dejó rastro -- self._execution sigue siendo el del constructor.
    assert qeng._execution.backend == "auto"


# =============================================================================================
# price_batch / price_many / price_grid -- paridad bit a bit contra el nativo.
# =============================================================================================


def _irs(notional, fixed_rate):
    return q.IRSwap(notional=notional, fixed_rate=fixed_rate, payment_times=[1.0, 2.0, 3.0], accruals=[1.0, 1.0, 1.0])


def test_price_batch_matches_the_native_price_batch():
    trades = [_irs(1_000_000.0, 0.02), _irs(2_000_000.0, 0.025), _irs(500_000.0, 0.018)]
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    market = q.Market(pillars=[1.0], zero_rates=[0.02])
    metrics = ["PV", "UnilateralCVA"]
    qeng = Engine(backend="cpu", n_paths=1_000, n_steps=1, seed=7)

    rows = qeng.price_batch(trades, model, market, metrics)

    eng = engine.Engine()
    native_products = [eng.create_product(t.product_type, t.to_params()) for t in trades]
    native_model = eng.create_model(model.model_type, model.to_params())
    native_market = engine.MarketSnapshot(**market.to_params())
    native_rows = eng.price_batch(
        native_products, _native_metrics(metrics), native_model, native_market,
        _native_pricing(1_000, 1, 7), _native_execution("cpu"),
    )

    assert len(rows) == len(native_rows) == 3
    for row, native_row in zip(rows, native_rows):
        assert row.trade_index == native_row.trade_index
        for name in metrics:
            _assert_measure_result_equal(row.measures[name], native_row.measures[name])


def test_price_many_matches_the_native_price_many_on_heterogeneous_trades():
    # price_many admite trades de distinto calendario (a diferencia de price_batch, que exige
    # el mismo calendario para todos) -- pero, igual que price_batch, cada IRSwap sigue
    # necesitando fixed_rate explícito (use_par_rate no soportado en ningún lote, ver
    # Engine.price_batch docstring en quantdesk/engine.py).
    trades = [
        _irs(1_000_000.0, 0.02),
        q.IRSwap(notional=750_000.0, fixed_rate=0.021, payment_times=[1.0, 2.0], accruals=[1.0, 1.0]),
        _irs(500_000.0, 0.018),
    ]
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    market = q.Market(pillars=[1.0], zero_rates=[0.02])
    metrics = ["PV"]
    qeng = Engine(backend="cpu", n_paths=1_000, n_steps=1, seed=7)

    rows = qeng.price_many(trades, model, market, metrics)

    eng = engine.Engine()
    native_products = [eng.create_product(t.product_type, t.to_params()) for t in trades]
    native_model = eng.create_model(model.model_type, model.to_params())
    native_market = engine.MarketSnapshot(**market.to_params())
    native_rows = eng.price_many(
        native_products, _native_metrics(metrics), native_model, native_market,
        _native_pricing(1_000, 1, 7), _native_execution("cpu"),
    )

    assert len(rows) == len(native_rows) == 3
    for row, native_row in zip(rows, native_rows):
        assert row.trade_index == native_row.trade_index
        _assert_measure_result_equal(row.measures["PV"], native_row.measures["PV"])


def test_price_grid_matches_the_native_price_grid():
    trades = [_irs(1_000_000.0, 0.02), _irs(2_000_000.0, 0.025)]
    models = [q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02), q.HullWhite2F(a=0.1, b=0.2, sigma=0.01, eta=0.012, rho=-0.7, r0=0.03)]
    markets = [q.Market(pillars=[1.0], zero_rates=[0.02]), q.Market(pillars=[1.0], zero_rates=[0.03])]
    metrics = ["PV"]
    qeng = Engine(backend="cpu", n_paths=1_000, n_steps=1, seed=7)

    cells = qeng.price_grid(trades, models, markets, metrics)

    eng = engine.Engine()
    native_products = [eng.create_product(t.product_type, t.to_params()) for t in trades]
    native_models = [eng.create_model(m.model_type, m.to_params()) for m in models]
    native_markets = [engine.MarketSnapshot(**mk.to_params()) for mk in markets]
    native_cells = eng.price_grid(
        native_products, _native_metrics(metrics), native_models, native_markets,
        _native_pricing(1_000, 1, 7), _native_execution("cpu"),
    )

    assert len(cells) == len(native_cells) == 2 * 2 * 2
    for cell, native_cell in zip(cells, native_cells):
        assert (cell.trade_index, cell.model_index, cell.market_index) == (
            native_cell.trade_index, native_cell.model_index, native_cell.market_index,
        )
        _assert_measure_result_equal(cell.measures["PV"], native_cell.measures["PV"])


# =============================================================================================
# all_greeks / hessian / hvp / simulate_paths / calibrate / list_* / Portfolio.
# =============================================================================================


def _gbm_call_case():
    trade = q.european_call("AAPL_CALL_100", "EQ.SPOT.AAPL", strike=100.0, notional=1_000.0, maturity=1.0)
    model = q.Gbm(s0=100.0, r=0.05, q=0.0, sigma=0.2, observable="EQ.SPOT.AAPL")
    market = q.Market(pillars=[1.0], zero_rates=[0.05])
    return trade, model, market


def _native_gbm_call_fixture(n_paths=50_000, seed=7):
    eng = engine.Engine()
    trade = q.european_call("AAPL_CALL_100", "EQ.SPOT.AAPL", strike=100.0, notional=1_000.0, maturity=1.0)
    product = eng.create_product(trade.product_type, trade.to_params())
    model = eng.create_model("GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"})
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.05])
    pricing = _native_pricing(n_paths, 1, seed)
    execution = _native_execution("cpu")
    return eng, product, model, market, pricing, execution


def test_all_greeks_matches_the_native_all_greeks():
    trade, model, market = _gbm_call_case()
    qeng = Engine(backend="cpu", n_paths=50_000, n_steps=1, seed=7)
    report = qeng.all_greeks(trade, "PayoffPriceQ", model, market)

    eng, product, native_model, native_market, pricing, execution = _native_gbm_call_fixture()
    native_report = eng.all_greeks(product, "PayoffPriceQ", native_model, native_market, pricing, execution)

    by_factor = {g.risk_factor: (g.value, g.measure, g.order) for g in report.greeks}
    native_by_factor = {g.risk_factor: (g.value, g.measure, g.order) for g in native_report.greeks}
    assert by_factor == native_by_factor
    assert report.skipped == native_report.skipped


def test_hessian_matches_the_native_hessian():
    trade, model, market = _gbm_call_case()
    qeng = Engine(backend="cpu", n_paths=50_000, n_steps=1, seed=7)
    report = qeng.hessian(trade, "PayoffPriceQ", model, market)

    eng, product, native_model, native_market, pricing, execution = _native_gbm_call_fixture()
    native_report = eng.hessian(product, "PayoffPriceQ", native_model, native_market, pricing, execution)

    pairs = {(e.factor_i, e.factor_j): e.value for e in report.entries}
    native_pairs = {(e.factor_i, e.factor_j): e.value for e in native_report.entries}
    assert pairs == native_pairs
    assert report.skipped == native_report.skipped


def test_hvp_matches_the_native_hvp():
    trade, model, market = _gbm_call_case()
    direction = {"model.spot": 1.0, "model.volatility": 0.0}
    qeng = Engine(backend="cpu", n_paths=50_000, n_steps=1, seed=7)
    report = qeng.hvp(trade, "PayoffPriceQ", model, market, direction)

    eng, product, native_model, native_market, pricing, execution = _native_gbm_call_fixture()
    native_report = eng.hvp(product, "PayoffPriceQ", native_model, native_market, pricing, execution, direction)

    components = {c.factor: c.value for c in report.components}
    native_components = {c.factor: c.value for c in native_report.components}
    assert components == native_components
    assert report.skipped == native_report.skipped


def test_simulate_paths_matches_the_native_simulate_paths():
    model = q.Gbm(s0=100.0, r=0.05, q=0.0, sigma=0.2, observable="EQ.SPOT.AAPL")
    market = q.Market(pillars=[2.0], zero_rates=[0.05])
    qeng = Engine(backend="cpu", n_paths=500, n_steps=8, seed=123)

    times, paths = qeng.simulate_paths(model, market)

    eng = engine.Engine()
    native_model = eng.create_model("GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"})
    native_market = engine.MarketSnapshot(pillars=[2.0], zero_rates=[0.05])
    native_times, native_paths = eng.simulate_paths(native_model, native_market, _native_pricing(500, 8, 123))

    import numpy as np
    assert np.array_equal(times, native_times)
    assert np.array_equal(paths, native_paths)


def test_simulate_paths_pointwise_pricing_override_does_not_mutate_the_engine():
    # Mismo criterio de no-mutación que price(...) (§2/Fase 2), aplicado a simulate_paths -- que
    # sí soporta pricing= puntual (PLAN_API_REFACTOR.md Fase 2, "Discrepancias resueltas").
    model = q.Gbm(s0=100.0, r=0.05, q=0.0, sigma=0.2, observable="EQ.SPOT.AAPL")
    market = q.Market(pillars=[1.0], zero_rates=[0.05])
    qeng = Engine(backend="cpu", n_paths=500, n_steps=8, seed=1)

    _, baseline_paths = qeng.simulate_paths(model, market)
    _, overridden_paths = qeng.simulate_paths(model, market, pricing=q.PricingContext(n_paths=500, n_steps=8, seed=999))

    import numpy as np
    assert not np.array_equal(baseline_paths, overridden_paths)  # override tuvo efecto real
    assert qeng._pricing == q.PricingContext(n_paths=500, n_steps=8, seed=1)  # sin mutar

    _, after_paths = qeng.simulate_paths(model, market)
    assert np.array_equal(after_paths, baseline_paths)  # llamada posterior sin override: baseline exacto


def test_calibrate_matches_the_native_create_calibrator_and_calibrate():
    market = q.Market(pillars=[0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0], zero_rates=[0.02] * 7)
    initial_guess = {"a": 0.2, "b": 0.02, "sigma": 0.01, "r0": 0.02}
    qeng = Engine(backend="cpu", n_paths=1, n_steps=1, seed=1)

    result = qeng.calibrate("HullWhite1F", market, initial_guess)

    eng = engine.Engine()
    native_market = engine.MarketSnapshot(**market.to_params())
    native_calibrator = eng.create_calibrator("HullWhite1F")
    native_result = native_calibrator.calibrate(native_market, initial_guess)

    assert result.converged == native_result.converged
    assert result.optimal_params == native_result.optimal_params


def test_list_methods_match_the_native_engine_registry():
    qeng = Engine(backend="cpu", n_paths=1, n_steps=1, seed=1)
    eng = engine.Engine()

    assert qeng.list_models() == eng.list_models()
    assert qeng.list_products() == eng.list_products()
    assert qeng.list_measures() == eng.list_measures()
    assert qeng.list_calibrators() == eng.list_calibrators()
    # Ninguna lista queda vacía -- confirma que el registry se pobló de verdad, no solo que
    # ambos lados devuelven "lo mismo" (podrían ser dos listas vacías idénticas).
    assert qeng.list_models() and qeng.list_products() and qeng.list_measures() and qeng.list_calibrators()


def test_portfolio_is_the_native_portfolio_reexported_directly():
    # §3.2: "Portfolio... se reexporta directamente desde engine.Portfolio sin envoltorio
    # adicional" -- mismo objeto, no una subclase ni una copia.
    assert q.Portfolio is engine.Portfolio


# =============================================================================================
# Gbm / GbmP (quantdesk.model, PLAN_API_REFACTOR.md Fase 5) -- sin test dedicado hasta ahora.
# =============================================================================================


def test_gbm_to_params_matches_the_native_gbm_params_and_feeds_the_real_engine():
    model = q.Gbm(s0=100.0, r=0.05, q=0.006, sigma=0.28, observable="EQ.SPOT.AAPL")
    assert model.model_type == "GBM"
    assert model.to_params() == {"s0": 100.0, "r": 0.05, "q": 0.006, "sigma": 0.28, "observable": "EQ.SPOT.AAPL"}

    eng = engine.Engine()
    native_model = eng.create_model(model.model_type, model.to_params())
    assert native_model.type_name == "GBM"


def test_gbm_p_to_params_matches_the_native_gbm_p_params_and_feeds_the_real_engine():
    model = q.GbmP(s0=100.0, mu=0.09, sigma=0.22, observable="EQ.SPOT.IDX")
    assert model.model_type == "GBM_P"
    assert model.to_params() == {"s0": 100.0, "mu": 0.09, "sigma": 0.22, "observable": "EQ.SPOT.IDX"}

    eng = engine.Engine()
    native_model = eng.create_model(model.model_type, model.to_params())
    assert native_model.type_name == "GBM_P"


def test_engine_price_with_gbm_matches_the_native_flow_bit_for_bit():
    trade, model, market = _gbm_call_case()
    qeng = Engine(backend="cpu", n_paths=100_000, n_steps=1, seed=11)
    result = qeng.price(trade, model, market, ["PayoffPriceQ"])

    eng, product, native_model, native_market, pricing, execution = _native_gbm_call_fixture(n_paths=100_000, seed=11)
    native_result = eng.price(product, ["PayoffPriceQ"], native_model, native_market, pricing, execution)

    _assert_measure_result_equal(result["PayoffPriceQ"], native_result["PayoffPriceQ"])


def test_engine_price_with_gbm_p_matches_the_native_flow_bit_for_bit():
    trade = q.PayoffProduct(
        id="FORECAST_TERMINAL_SPOT",
        contract=q.when(1.0, q.cashflow("USD", q.fixing("EQ.SPOT.IDX", 1.0))),
    )
    model = q.GbmP(s0=100.0, mu=0.09, sigma=0.22, observable="EQ.SPOT.IDX")
    market = q.Market(pillars=[1.0], zero_rates=[0.02])
    qeng = Engine(backend="cpu", n_paths=100_000, n_steps=1, seed=17)

    result = qeng.price(trade, model, market, ["PayoffForecastP"])

    eng = engine.Engine()
    product = eng.create_product(trade.product_type, trade.to_params())
    native_model = eng.create_model("GBM_P", {"s0": 100.0, "mu": 0.09, "sigma": 0.22, "observable": "EQ.SPOT.IDX"})
    native_market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02])
    native_result = eng.price(
        product, ["PayoffForecastP"], native_model, native_market, _native_pricing(100_000, 1, 17), _native_execution("cpu")
    )

    _assert_measure_result_equal(result["PayoffForecastP"], native_result["PayoffForecastP"])


# =============================================================================================
# Engine.evaluate_scenario (PLAN_API_REFACTOR.md Fase 5) -- sin test dedicado hasta ahora.
# `test_engine_evaluate_scenario.py` ya cubre EXHAUSTIVAMENTE `engine.Engine.evaluate_scenario`
# (el método nativo) para las 14 estrategias del notebook 09; aquí solo se confirma que el
# envoltorio tipado `quantdesk.Engine.evaluate_scenario` traduce trade->producto y delega
# idénticamente, sin reimplementar nada.
# =============================================================================================


def test_engine_evaluate_scenario_matches_the_native_evaluate_scenario():
    trade = q.custom_strategy(
        "LONG_BUTTERFLY",
        [
            q.call_leg("EQ.SPOT.TEST", 90.0, 1.0, 1.0),
            q.call_leg("EQ.SPOT.TEST", 100.0, -2.0, 1.0),
            q.call_leg("EQ.SPOT.TEST", 110.0, 1.0, 1.0),
        ],
    )
    qeng = Engine(backend="cpu", n_paths=1, n_steps=1, seed=1)

    eng = engine.Engine()
    native_product = eng.create_product(trade.product_type, trade.to_params())

    for spot in (60.0, 90.0, 100.0, 110.0, 140.0):
        scenario = {"EQ.SPOT.TEST": spot}
        ledger = qeng.evaluate_scenario(trade, scenario)
        native_ledger = eng.evaluate_scenario(native_product, scenario)
        assert ledger == native_ledger


def test_engine_evaluate_scenario_rejects_a_missing_observable_like_the_native_method():
    trade = q.european_call("AAPL_CALL_100", "EQ.SPOT.AAPL", strike=100.0, notional=1_000.0, maturity=1.0)
    qeng = Engine(backend="cpu", n_paths=1, n_steps=1, seed=1)
    with pytest.raises(Exception, match="fixing ausente"):
        qeng.evaluate_scenario(trade, {})


if __name__ == "__main__":
    test_price_matches_the_native_flow_bit_for_bit()
    test_price_result_dot_access_matches_bracket_access_and_is_the_same_object()
    test_price_result_dot_access_raises_attribute_error_for_a_measure_not_requested()
    test_price_override_pricing_has_a_real_effect_but_does_not_mutate_the_engine()
    test_price_override_execution_does_not_mutate_the_engine_for_subsequent_calls()
    test_price_batch_matches_the_native_price_batch()
    test_price_many_matches_the_native_price_many_on_heterogeneous_trades()
    test_price_grid_matches_the_native_price_grid()
    test_all_greeks_matches_the_native_all_greeks()
    test_hessian_matches_the_native_hessian()
    test_hvp_matches_the_native_hvp()
    test_simulate_paths_matches_the_native_simulate_paths()
    test_simulate_paths_pointwise_pricing_override_does_not_mutate_the_engine()
    test_calibrate_matches_the_native_create_calibrator_and_calibrate()
    test_list_methods_match_the_native_engine_registry()
    test_portfolio_is_the_native_portfolio_reexported_directly()
    test_gbm_to_params_matches_the_native_gbm_params_and_feeds_the_real_engine()
    test_gbm_p_to_params_matches_the_native_gbm_p_params_and_feeds_the_real_engine()
    test_engine_price_with_gbm_matches_the_native_flow_bit_for_bit()
    test_engine_price_with_gbm_p_matches_the_native_flow_bit_for_bit()
    test_engine_evaluate_scenario_matches_the_native_evaluate_scenario()
    test_engine_evaluate_scenario_rejects_a_missing_observable_like_the_native_method()
    print("OK: tests de quantdesk.Engine/PriceResult/BatchRow/GridRow/Gbm/GbmP/evaluate_scenario pasaron")
