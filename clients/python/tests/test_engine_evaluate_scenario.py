"""Tests de `Engine.evaluate_scenario` (PLAN_IMPROVE_NOTEBOOK2.md Fase 1: "Evaluacion
determinista de un contrato expuesta a Python"). NO es una medida de `Engine.price` -- es una
herramienta de notebook/diagnostico que ejecuta `ScenarioEvaluator` (AST C++ nativo) sobre un
escenario de mercado fijo (spot por observable), sin modelo, sin Monte Carlo, sin descuento --
ver `clients/python/notebooks/09_option_strategies_and_greeks.ipynb::intrinsic_value`, que este
motor sustituye.

Criterio de aceptacion exacto del plan: para las 14 estrategias de `09` sobre el mismo grid de
spot, `Engine.evaluate_scenario(...)` da un resultado IDENTICO (no solo "dentro de tolerancia":
es un calculo determinista) al que hoy produce `intrinsic_value(...)` en NumPy -- las 14
estrategias se reproducen aqui letra a letra (mismos kind/strike/qty/maturity que
STRATEGY_LEGS del notebook) para no depender de ejecutar el notebook en el test.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import engine  # noqa: E402
import quantdesk.payoff as q  # noqa: E402

OBS = "EQ.SPOT.TEST"
T = 1.0
T_NEAR = 0.5
T_FAR = 1.5

# Copia letra a letra de STRATEGY_LEGS en
# clients/python/notebooks/09_option_strategies_and_greeks.ipynb (unica fuente de verdad del
# notebook; duplicada aqui a proposito para que el test no dependa de ejecutar el notebook).
STRATEGY_LEGS = {
    "buy_call": [("call", 100.0, 1.0, T)],
    "sell_call": [("call", 100.0, -1.0, T)],
    "buy_put": [("put", 100.0, 1.0, T)],
    "sell_put": [("put", 100.0, -1.0, T)],
    "long_straddle": [("call", 100.0, 1.0, T), ("put", 100.0, 1.0, T)],
    "short_straddle": [("call", 100.0, -1.0, T), ("put", 100.0, -1.0, T)],
    "long_butterfly": [("call", 90.0, 1.0, T), ("call", 100.0, -2.0, T), ("call", 110.0, 1.0, T)],
    "short_butterfly": [("call", 90.0, -1.0, T), ("call", 100.0, 2.0, T), ("call", 110.0, -1.0, T)],
    "long_condor": [("call", 85.0, 1.0, T), ("call", 95.0, -1.0, T), ("call", 105.0, -1.0, T), ("call", 115.0, 1.0, T)],
    "short_condor": [("call", 85.0, -1.0, T), ("call", 95.0, 1.0, T), ("call", 105.0, 1.0, T), ("call", 115.0, -1.0, T)],
    "long_calendar_spread": [("call", 100.0, -1.0, T_NEAR), ("call", 100.0, 1.0, T_FAR)],
    "short_calendar_spread": [("call", 100.0, 1.0, T_NEAR), ("call", 100.0, -1.0, T_FAR)],
    "call_ratio_spread": [("call", 100.0, 1.0, T), ("call", 110.0, -2.0, T)],
    "short_ratio_spread": [("call", 100.0, -1.0, T), ("call", 110.0, 2.0, T)],
}

SPOT_GRID = np.linspace(60.0, 140.0, 41)


def intrinsic_value(legs, spot_grid):
    """Copia letra a letra de `intrinsic_value` del notebook 09 -- la referencia NumPy contra
    la que se compara `Engine.evaluate_scenario`, no una reimplementacion nueva."""
    total = np.zeros_like(spot_grid)
    for kind, strike, qty, _maturity in legs:
        if kind == "call":
            total = total + qty * np.maximum(spot_grid - strike, 0.0)
        else:
            total = total + qty * np.maximum(strike - spot_grid, 0.0)
    return total


def _legs_for(strategy_legs):
    return [(q.call_leg if kind == "call" else q.put_leg)(OBS, strike, qty, maturity) for kind, strike, qty, maturity in strategy_legs]


def _build_product(eng, name, strategy_legs):
    trade = q.custom_strategy(name.upper(), _legs_for(strategy_legs))
    return eng.create_product(trade.product_type, trade.to_params())


def _scenario_total(eng, product, spot):
    ledger = eng.evaluate_scenario(product, {OBS: float(spot)})
    currencies = {currency for _time, currency, _amount in ledger}
    assert currencies == {"USD"}, f"se esperaba una unica moneda USD, se obtuvo {currencies}"
    return sum(amount for _time, _currency, amount in ledger)


@pytest.mark.parametrize("name", list(STRATEGY_LEGS))
def test_evaluate_scenario_matches_numpy_intrinsic_value_exactly(name):
    eng = engine.Engine()
    strategy_legs = STRATEGY_LEGS[name]
    product = _build_product(eng, name, strategy_legs)

    expected = intrinsic_value(strategy_legs, SPOT_GRID)
    actual = np.array([_scenario_total(eng, product, spot) for spot in SPOT_GRID])

    # Exacto, no "dentro de tolerancia" (PLAN_IMPROVE_NOTEBOOK2.md Fase 1, criterio de
    # aceptacion explicito): ambos caminos son deterministas sobre la misma aritmetica de
    # punto flotante (suma/resta/max), sin ningun paso de Monte Carlo/aproximacion de por medio.
    np.testing.assert_array_equal(actual, expected)


def test_evaluate_scenario_rejects_a_required_observable_missing_from_the_scenario():
    eng = engine.Engine()
    product = _build_product(eng, "buy_call", STRATEGY_LEGS["buy_call"])
    with pytest.raises(Exception, match="fixing ausente"):
        eng.evaluate_scenario(product, {})


def test_evaluate_scenario_rejects_a_non_payoff_product():
    eng = engine.Engine()
    irs_product = eng.create_product(
        "IRSwap",
        {
            "notional": 1_000_000.0,
            "fixed_rate": 0.03,
            "start": 0.0,
            "payment_times": [1.0, 2.0],
            "accruals": [1.0, 1.0],
        },
    )
    with pytest.raises(Exception, match="producto no soportado"):
        eng.evaluate_scenario(irs_product, {"EQ.SPOT.TEST": 100.0})
