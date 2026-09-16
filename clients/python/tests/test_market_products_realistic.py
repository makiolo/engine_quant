"""Tests de producto de mercado realistas desde Python, fuera de PLAN_GREEKS.md: construye
combinaciones de productos tipicos (opciones vanilla, barreras, ejercicio americano/bermuda,
take-profit/stop-loss, forwards FX, swaps de tipos) con datos de mercado semi-realistas
(spot/strike/vol/tasas del orden de magnitud real) y ejercita, desde Python, las medidas que el
motor YA tiene cableadas a `Registry<IMeasure>`/`Engine.price(...)`: "PV"/"DV01"/
"ExpectedExposure"/"PFE95"/"UnilateralCVA" (IRS y, via `market_snapshot_bridge`, PayoffProduct
determinista) y las siete medidas Monte Carlo de payoff bajo Q/P (PLAN_PRODUCTS.md §12 Fase 5-7):
"PayoffPriceQ", "PayoffExerciseQ", "PayoffHitProbabilityQ", "PayoffExposureProfileQ",
"PayoffForecastP", "PayoffHitProbabilityP", "PayoffPnlDistributionP".

Deliberadamente NO ejercita "Greek"/"PayoffSensitivityQ" (PLAN_GREEKS.md) -- eso se cubre en
test_greeks.cpp/los tests C++ de sensibilidad; este archivo se queda en el lado "cuanto vale este
producto", no "cuanto cambia si muevo un parametro".

Salvo donde el motor garantiza una identidad EXACTA (put-call parity/knock-in+knock-out=vanilla
via numeros aleatorios comunes: mismo `seed`/`n_paths`/modelo en las dos-tres evaluaciones que se
comparan), las comprobaciones son de cota/signo/forma -- suficientes para confirmar que el
cableado funciona con datos realistas, no golden values frágiles a cambios de implementacion del
generador aleatorio.
"""

import math
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402
import engine_typed as q  # noqa: E402


# -------------------------------------------------------------------------------------------
# Infraestructura de mercado (semi-realista: ordenes de magnitud reales, no golden data externa)
# -------------------------------------------------------------------------------------------


def _gbm_q(s0, r, div_yield, sigma, observable):
    return {"s0": s0, "r": r, "q": div_yield, "sigma": sigma, "observable": observable}


def _gbm_p(s0, mu, sigma, observable):
    return {"s0": s0, "mu": mu, "sigma": sigma, "observable": observable}


def _flat_market(zero_rate=0.02, hazard_rate=0.0, recovery_rate=0.0):
    return engine.MarketSnapshot(pillars=[1.0], zero_rates=[zero_rate], hazard_rate=hazard_rate, recovery_rate=recovery_rate)


def _mc_pricing(n_paths, seed, n_steps=1.0):
    return engine.PricingContext({"pricing_date": 0.0, "n_paths": float(n_paths), "n_steps": n_steps, "seed": float(seed)})


def _cpu_execution():
    return engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})


# -------------------------------------------------------------------------------------------
# Plantillas de contrato locales (mismos patrones que cpp/engine/include/engine/payoff/
# barrier_templates.hpp / tp_sl.hpp / test_registry_wiring_payoff_measures.cpp::bermuda_put --
# no existen todavia como builders de alto nivel en engine_typed.payoff, asi que se replican aqui
# con los mismos nodos de AST/mismo criterio, en vez de esperar a que la libreria los exponga).
# -------------------------------------------------------------------------------------------


def _vanilla_call_contract(observable, strike, maturity):
    return q.when(maturity, q.cashflow("USD", q.maximum(q.fixing(observable, maturity) - strike, 0.0)))


def _vanilla_put_contract(observable, strike, maturity):
    return q.when(maturity, q.cashflow("USD", q.maximum(strike - q.fixing(observable, maturity), 0.0)))


def _up_and_in(event_id, observable, barrier, monitoring_times, underlying_contract):
    condition = q.greater_equal(q.current(observable), barrier)
    return q.trigger(event_id, monitoring_times, condition, "discrete", "at_hit", 0, True, underlying_contract, q.zero())


def _up_and_out(event_id, observable, barrier, monitoring_times, underlying_contract):
    condition = q.greater_equal(q.current(observable), barrier)
    return q.trigger(event_id, monitoring_times, condition, "discrete", "at_hit", 0, True, q.zero(), underlying_contract)


def _down_and_in(event_id, observable, barrier, monitoring_times, underlying_contract):
    condition = q.less_equal(q.current(observable), barrier)
    return q.trigger(event_id, monitoring_times, condition, "discrete", "at_hit", 0, True, underlying_contract, q.zero())


def _down_and_out(event_id, observable, barrier, monitoring_times, underlying_contract):
    condition = q.less_equal(q.current(observable), barrier)
    return q.trigger(event_id, monitoring_times, condition, "discrete", "at_hit", 0, True, q.zero(), underlying_contract)


def _double_knock_out(event_id, observable, barrier_low, barrier_high, monitoring_times, underlying_contract):
    spot = q.current(observable)
    condition = q.any_of([q.less_equal(spot, barrier_low), q.greater_equal(spot, barrier_high)])
    return q.trigger(event_id, monitoring_times, condition, "discrete", "at_hit", 0, True, q.zero(), underlying_contract)


def _bermuda_put_contract(observable, strike, maturity, exercise_dates):
    exercise_value = q.maximum(strike - q.current(observable), 0.0)
    continuation = _vanilla_put_contract(observable, strike, maturity)
    return q.exercise("EX", exercise_dates, exercise_value, continuation)


def _take_profit_stop_loss_contract(observable, entry_price, quantity, tp_return, sl_return, monitoring_times, currency="USD"):
    # Mismo patron que docs/schema/engine.payoff/examples/tp_sl.json, con entry_price/quantity
    # horneados como Constant en vez de Parameter: "parameter" no esta soportado por el
    # compilador IR que usan las medidas Monte Carlo GBM (rust/crates/engine-core/src/payoff/
    # compile.rs::compile_scalar), asi que un contrato que se va a PRECIAR (no solo explicar/
    # validar) tiene que hornear sus valores.
    ratio = q.div(q.current(observable), entry_price) - 1.0
    tp_condition = q.all_of([q.greater_equal(ratio, tp_return), q.negate(q.event_occurred("STOP_LOSS"))])
    sl_condition = q.all_of([q.less_equal(ratio, sl_return), q.negate(q.event_occurred("TAKE_PROFIT"))])
    tp_payoff = q.cashflow(currency, quantity * (q.event_value("TAKE_PROFIT", observable) - entry_price))
    sl_payoff = q.cashflow(currency, quantity * (q.event_value("STOP_LOSS", observable) - entry_price))
    take_profit = q.trigger("TAKE_PROFIT", monitoring_times, tp_condition, "discrete", "at_hit", 10, True, tp_payoff, q.zero())
    stop_loss = q.trigger("STOP_LOSS", monitoring_times, sl_condition, "discrete", "at_hit", 20, True, sl_payoff, q.zero())
    return q.both([take_profit, stop_loss])


def _cash_settled_fx_forward_contract(observable, notional_dom, forward_strike, maturity, currency_dom="USD"):
    # A diferencia de q.fx_forward (dos patas, dos monedas -- ver test abajo, fuera de alcance
    # para PV/DV01), un forward FX liquidado en efectivo en UNA sola moneda si es precariable
    # via PayoffPriceQ (Monte Carlo GBM sobre el tipo de cambio como "observable").
    return q.when(maturity, q.cashflow(currency_dom, notional_dom * (q.fixing(observable, maturity) - forward_strike)))


# =============================================================================================
# SECCION A -- opciones vanilla sobre indice/accion (Q), datos semi-realistas de un indice
# =============================================================================================

_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_T = 4500.0, 0.045, 0.013, 0.18, 1.0
_EQ_OBS = "EQ.SPOT.SPX"
_EQ_PATHS, _EQ_SEED = 200_000, 11


def _price_q(product, model_params, n_paths=_EQ_PATHS, seed=_EQ_SEED):
    eng = engine.Engine()
    model = eng.create_model("GBM", model_params)
    result = eng.price(product, ["PayoffPriceQ"], model, _flat_market(), _mc_pricing(n_paths, seed), _cpu_execution())
    return result["PayoffPriceQ"].scalar


def test_atm_call_price_is_positive_and_below_spot():
    contract = _vanilla_call_contract(_EQ_OBS, _EQ_S0, _EQ_T)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="SPX_ATM_CALL", contract=contract).to_params())
    price = _price_q(product, _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))
    assert 0.0 < price < _EQ_S0


def test_atm_put_price_is_positive_and_below_discounted_strike():
    contract = _vanilla_put_contract(_EQ_OBS, _EQ_S0, _EQ_T)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="SPX_ATM_PUT", contract=contract).to_params())
    price = _price_q(product, _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))
    assert 0.0 < price < _EQ_S0 * math.exp(-_EQ_R * _EQ_T)


def test_put_call_parity_holds_within_monte_carlo_tolerance():
    # C - P == S0*e^-qT - K*e^-rT: cada medida se evalua por separado (dos llamadas a
    # Engine.price, no bump-and-reval con numeros aleatorios comunes -- eso es PLAN_GREEKS.md
    # §4.4, fuera de alcance aqui), asi que la identidad se cumple solo dentro del ruido Monte
    # Carlo de dos simulaciones independientes, no a precision de maquina. n_paths alto (1M)
    # mantiene ese ruido pequeno frente al propio precio (~450).
    eng = engine.Engine()
    model = eng.create_model("GBM", _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))
    strike = _EQ_S0 * 0.97
    call_product = eng.create_product(
        "Payoff", q.PayoffProduct(id="C", contract=_vanilla_call_contract(_EQ_OBS, strike, _EQ_T)).to_params()
    )
    put_product = eng.create_product(
        "Payoff", q.PayoffProduct(id="P", contract=_vanilla_put_contract(_EQ_OBS, strike, _EQ_T)).to_params()
    )
    pricing = _mc_pricing(1_000_000, _EQ_SEED)
    call_price = eng.price(call_product, ["PayoffPriceQ"], model, _flat_market(), pricing, _cpu_execution())["PayoffPriceQ"].scalar
    put_price = eng.price(put_product, ["PayoffPriceQ"], model, _flat_market(), pricing, _cpu_execution())["PayoffPriceQ"].scalar

    parity_rhs = _EQ_S0 * math.exp(-_EQ_Q * _EQ_T) - strike * math.exp(-_EQ_R * _EQ_T)
    assert math.isclose(call_price - put_price, parity_rhs, abs_tol=5.0)


def test_deep_itm_call_price_is_close_to_discounted_forward_intrinsic():
    strike = _EQ_S0 * 0.5  # tan dentro de dinero que casi siempre se ejerce
    contract = _vanilla_call_contract(_EQ_OBS, strike, _EQ_T)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="SPX_DEEP_ITM_CALL", contract=contract).to_params())
    price = _price_q(product, _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))
    intrinsic_forward = _EQ_S0 * math.exp(-_EQ_Q * _EQ_T) - strike * math.exp(-_EQ_R * _EQ_T)
    assert math.isclose(price, intrinsic_forward, rel_tol=0.02)


def test_deep_otm_put_price_is_near_zero():
    strike = _EQ_S0 * 0.5  # tan fuera de dinero que casi nunca se ejerce
    contract = _vanilla_put_contract(_EQ_OBS, strike, _EQ_T)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="SPX_DEEP_OTM_PUT", contract=contract).to_params())
    price = _price_q(product, _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))
    assert price < 0.001 * _EQ_S0


# =============================================================================================
# SECCION B -- opciones barrera sobre una accion realista (Q)
# =============================================================================================

_STK_S0, _STK_R, _STK_Q, _STK_SIGMA, _STK_T = 180.0, 0.05, 0.006, 0.28, 1.0
_STK_OBS = "EQ.SPOT.AAPL"
_STK_MONITORING = [0.25, 0.5, 0.75, 1.0]
_STK_PATHS, _STK_SEED = 150_000, 23


def _stock_model():
    return _gbm_q(_STK_S0, _STK_R, _STK_Q, _STK_SIGMA, _STK_OBS)


def test_up_and_in_call_price_is_between_zero_and_vanilla_price():
    barrier = _STK_S0 * 1.15
    vanilla = _vanilla_call_contract(_STK_OBS, _STK_S0, _STK_T)
    knock_in = _up_and_in("UI", _STK_OBS, barrier, _STK_MONITORING, vanilla)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="AAPL_UI_CALL", contract=knock_in).to_params())
    vanilla_product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="AAPL_CALL", contract=vanilla).to_params())

    ui_price = _price_q(product, _stock_model(), n_paths=_STK_PATHS, seed=_STK_SEED)
    vanilla_price = _price_q(vanilla_product, _stock_model(), n_paths=_STK_PATHS, seed=_STK_SEED)
    assert 0.0 < ui_price < vanilla_price


def test_up_and_in_plus_up_and_out_reproduces_the_vanilla_call_price():
    # Igual que payoff::api::tests::up_and_in_plus_up_and_out_reproduces_the_vanilla_call_price
    # (Rust) pero desde Python/Engine.price: knock-in + knock-out == vanilla, exactamente una de
    # las dos ramas paga en cada trayectoria -- pero cada medida es una llamada independiente a
    # Engine.price (no numeros aleatorios comunes explicitos, PLAN_GREEKS.md §4.4), asi que la
    # identidad se cumple dentro de ruido Monte Carlo, no a precision de maquina; n_paths alto
    # (500k) mantiene ese ruido bien por debajo del precio (~23-24).
    eng = engine.Engine()
    model = eng.create_model("GBM", _stock_model())
    barrier = _STK_S0 * 1.15
    vanilla = _vanilla_call_contract(_STK_OBS, _STK_S0, _STK_T)
    knock_in = _up_and_in("UI", _STK_OBS, barrier, _STK_MONITORING, vanilla)
    knock_out = _up_and_out("UO", _STK_OBS, barrier, _STK_MONITORING, vanilla)

    pricing = _mc_pricing(500_000, _STK_SEED)
    market, execution = _flat_market(), _cpu_execution()

    def price(contract, product_id):
        product = eng.create_product("Payoff", q.PayoffProduct(id=product_id, contract=contract).to_params())
        return eng.price(product, ["PayoffPriceQ"], model, market, pricing, execution)["PayoffPriceQ"].scalar

    assert math.isclose(price(knock_in, "UI") + price(knock_out, "UO"), price(vanilla, "VANILLA"), abs_tol=1.0)


def test_down_and_in_plus_down_and_out_reproduces_the_vanilla_put_price():
    eng = engine.Engine()
    model = eng.create_model("GBM", _stock_model())
    barrier = _STK_S0 * 0.80
    vanilla = _vanilla_put_contract(_STK_OBS, _STK_S0, _STK_T)
    knock_in = _down_and_in("DI", _STK_OBS, barrier, _STK_MONITORING, vanilla)
    knock_out = _down_and_out("DO", _STK_OBS, barrier, _STK_MONITORING, vanilla)

    pricing = _mc_pricing(500_000, _STK_SEED)
    market, execution = _flat_market(), _cpu_execution()

    def price(contract, product_id):
        product = eng.create_product("Payoff", q.PayoffProduct(id=product_id, contract=contract).to_params())
        return eng.price(product, ["PayoffPriceQ"], model, market, pricing, execution)["PayoffPriceQ"].scalar

    assert math.isclose(price(knock_in, "DI") + price(knock_out, "DO"), price(vanilla, "VANILLA"), abs_tol=1.0)


def test_up_barrier_hit_probability_is_between_zero_and_one():
    barrier = _STK_S0 * 1.15
    vanilla = _vanilla_call_contract(_STK_OBS, _STK_S0, _STK_T)
    knock_in = _up_and_in("UI", _STK_OBS, barrier, _STK_MONITORING, vanilla)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="AAPL_UI_PROB", contract=knock_in).to_params())

    eng = engine.Engine()
    model = eng.create_model("GBM", _stock_model())
    result = eng.price(
        product, [("PayoffHitProbabilityQ", {"event": "UI"})], model, _flat_market(),
        _mc_pricing(_STK_PATHS, _STK_SEED), _cpu_execution(),
    )
    probability = result["PayoffHitProbabilityQ"].scalar
    assert 0.0 <= probability <= 1.0
    assert probability > 0.05  # con sigma=28%/1y y barrera a +15%, no deberia ser un suceso raro


def test_double_knock_out_corridor_is_cheaper_than_the_vanilla_call():
    low, high = _STK_S0 * 0.80, _STK_S0 * 1.25
    vanilla = _vanilla_call_contract(_STK_OBS, _STK_S0, _STK_T)
    corridor = _double_knock_out("KO", _STK_OBS, low, high, _STK_MONITORING, vanilla)
    corridor_product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="AAPL_KO_CALL", contract=corridor).to_params())
    vanilla_product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="AAPL_CALL", contract=vanilla).to_params())

    corridor_price = _price_q(corridor_product, _stock_model(), n_paths=_STK_PATHS, seed=_STK_SEED)
    vanilla_price = _price_q(vanilla_product, _stock_model(), n_paths=_STK_PATHS, seed=_STK_SEED)
    assert 0.0 <= corridor_price < vanilla_price


# =============================================================================================
# SECCION C -- ejercicio bermuda (Q), Longstaff-Schwartz
# =============================================================================================

_BERM_S0, _BERM_STRIKE, _BERM_R, _BERM_Q, _BERM_SIGMA, _BERM_T = 100.0, 100.0, 0.04, 0.0, 0.30, 1.0
_BERM_OBS = "EQ.SPOT.IDX"
_BERM_DATES = [0.25, 0.5, 0.75]
_BERM_PATHS, _BERM_SEED = 100_000, 41


def test_bermudan_put_price_is_at_least_the_european_price():
    european = _vanilla_put_contract(_BERM_OBS, _BERM_STRIKE, _BERM_T)
    bermudan = _bermuda_put_contract(_BERM_OBS, _BERM_STRIKE, _BERM_T, _BERM_DATES)
    model_params = _gbm_q(_BERM_S0, _BERM_R, _BERM_Q, _BERM_SIGMA, _BERM_OBS)

    european_product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="EU_PUT", contract=european).to_params())
    bermudan_product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="BERM_PUT", contract=bermudan).to_params())

    european_price = _price_q(european_product, model_params, n_paths=_BERM_PATHS, seed=_BERM_SEED)

    eng = engine.Engine()
    model = eng.create_model("GBM", model_params)
    bermudan_price = eng.price(
        bermudan_product, ["PayoffExerciseQ"], model, _flat_market(), _mc_pricing(_BERM_PATHS, _BERM_SEED), _cpu_execution()
    )["PayoffExerciseQ"].scalar

    # Longstaff-Schwartz es una politica subóptima estimada por regresion: se permite un pequeño
    # margen bajo la cota teorica (bermuda >= europea) en vez de exigir la desigualdad exacta.
    assert bermudan_price >= european_price - 0.05 * european_price


def test_bermudan_put_exercise_diagnostics_have_one_row_per_decision_date():
    bermudan = _bermuda_put_contract(_BERM_OBS, _BERM_STRIKE, _BERM_T, _BERM_DATES)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="BERM_PUT", contract=bermudan).to_params())
    eng = engine.Engine()
    model = eng.create_model("GBM", _gbm_q(_BERM_S0, _BERM_R, _BERM_Q, _BERM_SIGMA, _BERM_OBS))

    result = eng.price(
        product, ["PayoffExerciseQ"], model, _flat_market(), _mc_pricing(_BERM_PATHS, _BERM_SEED), _cpu_execution()
    )["PayoffExerciseQ"]

    assert result.has_scalar and result.scalar > 0.0
    assert list(result.times) == _BERM_DATES
    assert len(result.primary) == len(_BERM_DATES)
    assert len(result.secondary) == len(_BERM_DATES)
    for exercised_fraction in result.primary:
        assert 0.0 <= exercised_fraction <= 1.0


# =============================================================================================
# SECCION D -- take-profit/stop-loss (Q), estrategia intradia/swing tipica
# =============================================================================================


def test_take_profit_stop_loss_price_is_finite_and_within_plausible_pnl_bounds():
    entry_price, quantity = 250.0, 1_000.0
    tp_return, sl_return = 0.15, -0.08
    monitoring = [t / 12.0 for t in range(1, 13)]  # revision mensual durante 1 ano
    contract = _take_profit_stop_loss_contract(
        "EQ.SPOT.NVDA", entry_price, quantity, tp_return, sl_return, monitoring
    )
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="NVDA_TP_SL", contract=contract).to_params())
    price = _price_q(
        product, _gbm_q(entry_price, 0.05, 0.0, 0.35, "EQ.SPOT.NVDA"), n_paths=150_000, seed=71
    )
    # El payoff maximo posible (en valor absoluto) esta acotado por quantity * max(|tp|, |sl|) *
    # entry_price, mas margen por gaps que sobrepasan el nivel exacto de disparo.
    max_plausible = quantity * entry_price * max(abs(tp_return), abs(sl_return)) * 1.5
    assert math.isfinite(price)
    assert abs(price) < max_plausible


# =============================================================================================
# SECCION E -- perfil de exposicion (Q) de una opcion frente a una contraparte
# =============================================================================================


def test_equity_call_exposure_profile_is_nonnegative_and_pfe_at_least_ee():
    contract = _vanilla_call_contract(_EQ_OBS, _EQ_S0, _EQ_T)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="SPX_CALL_EXPOSURE", contract=contract).to_params())
    eng = engine.Engine()
    model = eng.create_model("GBM", _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))
    exposure_times = [0.0, 0.25, 0.5, 0.75, 1.0]

    result = eng.price(
        product, [("PayoffExposureProfileQ", {"exposure_times": exposure_times})], model, _flat_market(),
        _mc_pricing(100_000, 5), _cpu_execution(),
    )["PayoffExposureProfileQ"]

    assert not result.has_scalar
    assert list(result.times) == exposure_times
    for ee, pfe in zip(result.primary, result.secondary):
        assert ee >= 0.0
        assert pfe >= ee


# =============================================================================================
# SECCION F -- medida fisica P: forecast, probabilidad de hit y distribucion de P&L
# =============================================================================================

_P_S0, _P_MU, _P_SIGMA, _P_T = 100.0, 0.09, 0.22, 1.0  # prima de riesgo de renta variable tipica ~9%
_P_OBS = "EQ.SPOT.IDX"
_P_PATHS, _P_SEED = 200_000, 17


def test_forecast_p_mean_matches_the_analytic_physical_drift():
    contract = q.when(_P_T, q.cashflow("USD", q.fixing(_P_OBS, _P_T)))
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="FORECAST_TERMINAL_SPOT", contract=contract).to_params())
    eng = engine.Engine()
    model = eng.create_model("GBM_P", _gbm_p(_P_S0, _P_MU, _P_SIGMA, _P_OBS))

    result = eng.price(product, ["PayoffForecastP"], model, _flat_market(), _mc_pricing(_P_PATHS, _P_SEED), _cpu_execution())
    forecast_mean = result["PayoffForecastP"].scalar
    analytic = _P_S0 * math.exp(_P_MU * _P_T)
    assert math.isclose(forecast_mean, analytic, rel_tol=0.02)


def test_forecast_p_rejects_a_risk_neutral_model():
    # El motor rechaza combinaciones Q/P invalidas (PLAN_PRODUCTS.md §12 Fase 7) -- pedir una
    # metrica P con un modelo GBM (solo declara Q) falla explicito, no degrada en silencio.
    contract = q.when(_P_T, q.cashflow("USD", q.fixing(_P_OBS, _P_T)))
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="FORECAST_TERMINAL_SPOT", contract=contract).to_params())
    eng = engine.Engine()
    model = eng.create_model("GBM", _gbm_q(_P_S0, 0.04, 0.0, _P_SIGMA, _P_OBS))

    try:
        eng.price(product, ["PayoffForecastP"], model, _flat_market(), _mc_pricing(1_000, 1), _cpu_execution())
        raise AssertionError("se esperaba una excepcion (modelo Q sobre una metrica P)")
    except AssertionError:
        raise
    except Exception as e:  # noqa: BLE001 -- el tipo exacto que cruza nanobind no esta documentado aqui
        assert "no soportado" in str(e)


def test_hit_probability_p_of_a_realistic_upside_barrier_is_between_zero_and_one():
    barrier = _P_S0 * 1.20
    monitoring = [0.25, 0.5, 0.75, 1.0]
    vanilla = _vanilla_call_contract(_P_OBS, _P_S0, _P_T)
    knock_in = _up_and_in("UI", _P_OBS, barrier, monitoring, vanilla)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="IDX_UI_PHYSICAL", contract=knock_in).to_params())

    eng = engine.Engine()
    model = eng.create_model("GBM_P", _gbm_p(_P_S0, _P_MU, _P_SIGMA, _P_OBS))
    result = eng.price(
        product, [("PayoffHitProbabilityP", {"event": "UI"})], model, _flat_market(),
        _mc_pricing(_P_PATHS, _P_SEED), _cpu_execution(),
    )
    probability = result["PayoffHitProbabilityP"].scalar
    assert 0.0 <= probability <= 1.0


def test_pnl_distribution_p_of_a_long_stock_position_reports_es_at_least_var_and_matches_analytic_mean():
    notional = 10_000.0
    contract = q.when(_P_T, q.cashflow("USD", notional * (q.fixing(_P_OBS, _P_T) / _P_S0 - 1.0)))
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="LONG_IDX_PNL", contract=contract).to_params())
    eng = engine.Engine()
    model = eng.create_model("GBM_P", _gbm_p(_P_S0, _P_MU, _P_SIGMA, _P_OBS))

    result = eng.price(
        product, [("PayoffPnlDistributionP", {"confidence": 0.95})], model, _flat_market(),
        _mc_pricing(_P_PATHS, _P_SEED), _cpu_execution(),
    )["PayoffPnlDistributionP"]

    assert result.has_scalar
    assert len(result.primary) == 1 and len(result.secondary) == 1
    var, expected_shortfall = result.primary[0], result.secondary[0]
    assert expected_shortfall >= var  # ES al menos tan severo como VaR (PLAN_PRODUCTS.md §12 Fase 7)

    analytic_mean_return = math.exp(_P_MU * _P_T) - 1.0
    assert math.isclose(result.scalar, notional * analytic_mean_return, rel_tol=0.05)


# =============================================================================================
# SECCION G -- ledger determinista (PV/DV01) de un producto Payoff, contrastado contra IRSwap
# nativo (PLAN_PRODUCTS.md §9.1/§9.5): el mismo swap construido de dos formas debe coincidir.
# =============================================================================================


def _realistic_5y_swap_schedule():
    return {"payment_times": [1.0, 2.0, 3.0, 4.0, 5.0], "accruals": [1.0, 1.0, 1.0, 1.0, 1.0]}


def test_payoff_style_irs_pv_matches_the_native_irswap_pv():
    notional, fixed_rate = 10_000_000.0, 0.038
    schedule = _realistic_5y_swap_schedule()
    eng = engine.Engine()
    model = eng.create_model("GBM", _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))  # ignorado por PV/DV01 de Payoff
    market = _flat_market(zero_rate=0.038)
    pricing = _mc_pricing(1, 1)
    execution = _cpu_execution()

    payoff_swap = eng.create_product(
        "Payoff", q.irs("PAYOFF_5Y_SWAP", notional=notional, fixed_rate=fixed_rate, **schedule).to_params()
    )
    native_swap = eng.create_product("IRSwap", {"notional": notional, "fixed_rate": fixed_rate, **schedule})

    payoff_pv = eng.price(payoff_swap, ["PV"], model, market, pricing, execution)["PV"].scalar
    native_pv = eng.price(native_swap, ["PV"], model, market, pricing, execution)["PV"].scalar
    assert math.isclose(payoff_pv, native_pv, abs_tol=1e-6)


def test_payoff_style_irs_dv01_matches_the_native_irswap_dv01():
    notional, fixed_rate = 10_000_000.0, 0.038
    schedule = _realistic_5y_swap_schedule()
    eng = engine.Engine()
    model = eng.create_model("GBM", _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))
    market = _flat_market(zero_rate=0.038)
    pricing = _mc_pricing(1, 1)
    execution = _cpu_execution()

    payoff_swap = eng.create_product(
        "Payoff", q.irs("PAYOFF_5Y_SWAP", notional=notional, fixed_rate=fixed_rate, **schedule).to_params()
    )
    native_swap = eng.create_product("IRSwap", {"notional": notional, "fixed_rate": fixed_rate, **schedule})

    payoff_dv01 = eng.price(payoff_swap, ["DV01"], model, market, pricing, execution)["DV01"].scalar
    native_dv01 = eng.price(native_swap, ["DV01"], model, market, pricing, execution)["DV01"].scalar
    assert math.isclose(payoff_dv01, native_dv01, abs_tol=1e-6)


# =============================================================================================
# SECCION H -- FX: la plantilla de dos monedas se rechaza para PV/DV01; un forward liquidado en
# una sola moneda si se puede preciar via Monte Carlo GBM.
# =============================================================================================


def test_two_currency_fx_forward_present_value_is_rejected_explicitly():
    # market_snapshot_bridge.hpp: "un payoff fuera de este alcance (mas de una moneda...) es
    # std::invalid_argument explicito" -- q.fx_forward paga EUR en una pata y USD en la otra.
    trade = q.fx_forward("EURUSD_FWD", "EUR", "USD", 1_000_000.0, 1.09, 0.5)
    eng = engine.Engine()
    product = eng.create_product(trade.product_type, trade.to_params())
    model = eng.create_model("GBM", _gbm_q(_EQ_S0, _EQ_R, _EQ_Q, _EQ_SIGMA, _EQ_OBS))

    try:
        eng.price(product, ["PV"], model, _flat_market(), _mc_pricing(1, 1), _cpu_execution())
        raise AssertionError("se esperaba una excepcion (mas de una moneda en el ledger determinista)")
    except AssertionError:
        raise
    except Exception as e:  # noqa: BLE001
        assert "moneda" in str(e)


def test_cash_settled_fx_forward_price_is_close_to_covered_interest_parity():
    # NDF-like: liquida en USD la diferencia entre el spot a vencimiento y un strike forward,
    # sobre un GBM sin dividendos explicitos (carry FX: r=domestica, q=extranjera).
    s0, r_dom, r_for, sigma, maturity = 1.0850, 0.038, 0.032, 0.075, 0.5
    forward_strike = s0 * math.exp((r_dom - r_for) * maturity)  # paridad cubierta de tipos
    notional_dom = 1_000_000.0
    contract = _cash_settled_fx_forward_contract("FX.EURUSD", notional_dom, forward_strike, maturity)
    product = engine.Engine().create_product("Payoff", q.PayoffProduct(id="EURUSD_NDF", contract=contract).to_params())

    eng = engine.Engine()
    model = eng.create_model("GBM", _gbm_q(s0, r_dom, r_for, sigma, "FX.EURUSD"))
    price = eng.price(
        product, ["PayoffPriceQ"], model, _flat_market(zero_rate=r_dom), _mc_pricing(200_000, 3), _cpu_execution()
    )["PayoffPriceQ"].scalar

    # Bajo GBM con drift r_dom-r_for, E_Q[S_T] = s0*e^{(r_dom-r_for)*T} = forward_strike, asi que
    # el valor no descontado deberia ser ~0; descontado sigue siendo ~0 (dentro de ruido MC).
    assert abs(price) < 0.01 * notional_dom * sigma * math.sqrt(maturity)


# =============================================================================================
# SECCION I -- swap de tipos nativo con datos semi-realistas (multi-pillar, credito no nulo)
# =============================================================================================

_SWAP_PILLARS = [0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0]
_SWAP_ZERO_RATES = [0.031, 0.032, 0.034, 0.0355, 0.038, 0.0395, 0.0405]  # curva realista con pendiente positiva


def _upward_sloping_curve(hazard_rate=0.0, recovery_rate=0.0):
    return engine.MarketSnapshot(
        pillars=_SWAP_PILLARS, zero_rates=_SWAP_ZERO_RATES, hazard_rate=hazard_rate, recovery_rate=recovery_rate
    )


def _hull_white_1f():
    return {"a": 0.03, "b": 0.035, "sigma": 0.008, "r0": 0.032}


def test_realistic_10y_swap_bucketed_dv01_sums_to_the_parallel_dv01():
    payment_times = [1.0, 2.0, 3.0, 5.0, 7.0, 10.0]
    accruals = [1.0, 1.0, 1.0, 2.0, 2.0, 3.0]
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_1f())
    product = eng.create_product(
        "IRSwap", {"notional": 25_000_000.0, "fixed_rate": 0.039, "payment_times": payment_times, "accruals": accruals}
    )
    market = _upward_sloping_curve()
    pricing = _mc_pricing(1, 1)
    execution = _cpu_execution()

    parallel = eng.price(product, [("DV01", {"bucketed": False})], model, market, pricing, execution)["DV01"]
    bucketed = eng.price(product, [("DV01", {"bucketed": True})], model, market, pricing, execution)["DV01"]

    assert parallel.has_scalar
    assert not bucketed.has_scalar
    assert len(bucketed.primary) == len(_SWAP_PILLARS)
    assert math.isclose(sum(bucketed.primary), parallel.scalar, rel_tol=1e-6, abs_tol=1e-3)


def test_receiver_swap_has_opposite_sign_dv01_from_payer_swap():
    schedule = _realistic_5y_swap_schedule()
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_1f())
    market, pricing, execution = _upward_sloping_curve(), _mc_pricing(1, 1), _cpu_execution()

    payer = eng.create_product("IRSwap", {"notional": 10_000_000.0, "fixed_rate": 0.037, **schedule})
    receiver = eng.create_product("IRSwap", {"notional": -10_000_000.0, "fixed_rate": 0.037, **schedule})

    payer_dv01 = eng.price(payer, ["DV01"], model, market, pricing, execution)["DV01"].scalar
    receiver_dv01 = eng.price(receiver, ["DV01"], model, market, pricing, execution)["DV01"].scalar
    assert math.isclose(receiver_dv01, -payer_dv01, rel_tol=1e-9)


def test_realistic_swap_full_measure_suite_reports_consistent_signs_and_shapes():
    schedule = _realistic_5y_swap_schedule()
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_1f())
    product = eng.create_product("IRSwap", {"notional": 15_000_000.0, "fixed_rate": 0.036, **schedule})
    market = _upward_sloping_curve(hazard_rate=0.015, recovery_rate=0.40)  # credito single-A tipico
    pricing = _mc_pricing(20_000, 9)
    execution = _cpu_execution()

    result = eng.price(product, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"], model, market, pricing, execution)

    assert result["PV"].has_scalar
    assert result["DV01"].has_scalar
    ee, pfe, cva = result["ExpectedExposure"], result["PFE95"], result["UnilateralCVA"]
    assert len(ee.primary) == len(schedule["payment_times"])
    for ee_i, pfe_i in zip(ee.primary, pfe.primary):
        assert ee_i >= 0.0
        assert pfe_i >= ee_i
    assert cva.has_scalar
    assert cva.scalar > 0.0  # hazard_rate>0 y exposicion no nula -> CVA estrictamente positivo


def test_unilateral_cva_is_zero_when_hazard_rate_is_zero():
    schedule = _realistic_5y_swap_schedule()
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_1f())
    product = eng.create_product("IRSwap", {"notional": 15_000_000.0, "fixed_rate": 0.036, **schedule})
    pricing, execution = _mc_pricing(20_000, 9), _cpu_execution()

    cva_with_credit = eng.price(
        product, ["UnilateralCVA"], model, _upward_sloping_curve(hazard_rate=0.02, recovery_rate=0.4), pricing, execution
    )["UnilateralCVA"].scalar
    cva_without_credit = eng.price(
        product, ["UnilateralCVA"], model, _upward_sloping_curve(hazard_rate=0.0, recovery_rate=0.4), pricing, execution
    )["UnilateralCVA"].scalar

    assert cva_without_credit == 0.0
    assert cva_with_credit > cva_without_credit


if __name__ == "__main__":
    test_atm_call_price_is_positive_and_below_spot()
    test_atm_put_price_is_positive_and_below_discounted_strike()
    test_put_call_parity_holds_within_monte_carlo_tolerance()
    test_deep_itm_call_price_is_close_to_discounted_forward_intrinsic()
    test_deep_otm_put_price_is_near_zero()
    test_up_and_in_call_price_is_between_zero_and_vanilla_price()
    test_up_and_in_plus_up_and_out_reproduces_the_vanilla_call_price()
    test_down_and_in_plus_down_and_out_reproduces_the_vanilla_put_price()
    test_up_barrier_hit_probability_is_between_zero_and_one()
    test_double_knock_out_corridor_is_cheaper_than_the_vanilla_call()
    test_bermudan_put_price_is_at_least_the_european_price()
    test_bermudan_put_exercise_diagnostics_have_one_row_per_decision_date()
    test_take_profit_stop_loss_price_is_finite_and_within_plausible_pnl_bounds()
    test_equity_call_exposure_profile_is_nonnegative_and_pfe_at_least_ee()
    test_forecast_p_mean_matches_the_analytic_physical_drift()
    test_forecast_p_rejects_a_risk_neutral_model()
    test_hit_probability_p_of_a_realistic_upside_barrier_is_between_zero_and_one()
    test_pnl_distribution_p_of_a_long_stock_position_reports_es_at_least_var_and_matches_analytic_mean()
    test_payoff_style_irs_pv_matches_the_native_irswap_pv()
    test_payoff_style_irs_dv01_matches_the_native_irswap_dv01()
    test_two_currency_fx_forward_present_value_is_rejected_explicitly()
    test_cash_settled_fx_forward_price_is_close_to_covered_interest_parity()
    test_realistic_10y_swap_bucketed_dv01_sums_to_the_parallel_dv01()
    test_receiver_swap_has_opposite_sign_dv01_from_payer_swap()
    test_realistic_swap_full_measure_suite_reports_consistent_signs_and_shapes()
    test_unilateral_cva_is_zero_when_hazard_rate_is_zero()
    print("OK: tests de productos de mercado realistas (fuera de PLAN_GREEKS.md) pasaron")
