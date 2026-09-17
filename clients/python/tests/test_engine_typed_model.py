"""Tests de `engine_typed.model.GbmBasket` (PLAN_IMPROVE_NOTEBOOK.md Fase 3): validación temprana
de la matriz de correlación (simetría/PSD, mismo criterio que `engine_typed.market.Market` valida
pillars crecientes) y round-trip contra el `Engine` real -- un basket call de 2 activos
correlacionados se precia, y su precio se mueve en la dirección correcta al variar la correlación
(criterio de aceptación explícito de esta fase, PLAN_IMPROVE_NOTEBOOK.md §2 Fase 3).
"""

import sys
from pathlib import Path

import pytest

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402
import engine_typed as q  # noqa: E402
from engine_typed.model import GbmBasket  # noqa: E402


def _basket_spec(correlation, s0=(100.0, 100.0), r=(0.03, 0.03), sigma=(0.25, 0.25)):
    return GbmBasket(
        observables=["EQ.SPOT.A", "EQ.SPOT.B"],
        s0=list(s0),
        r=list(r),
        q=[0.0, 0.0],
        sigma=list(sigma),
        correlation=correlation,
    )


def test_to_params_flattens_correlation_row_major_and_joins_observables():
    spec = _basket_spec([[1.0, 0.4], [0.4, 1.0]])
    params = spec.to_params()
    assert params["observables"] == ["EQ.SPOT.A", "EQ.SPOT.B"]
    assert params["correlation"] == [1.0, 0.4, 0.4, 1.0]
    assert params["s0"] == [100.0, 100.0]


def test_rejects_mismatched_vector_lengths():
    with pytest.raises(ValueError, match="misma longitud"):
        GbmBasket(observables=["A", "B"], s0=[100.0], r=[0.03, 0.03], q=[0.0, 0.0], sigma=[0.2, 0.2],
                  correlation=[[1.0, 0.0], [0.0, 1.0]])


def test_rejects_non_square_correlation():
    with pytest.raises(ValueError, match="matriz"):
        _basket_spec([[1.0, 0.4, 0.0], [0.4, 1.0, 0.0]])


def test_rejects_asymmetric_correlation():
    with pytest.raises(ValueError, match="simétrica"):
        _basket_spec([[1.0, 0.4], [0.5, 1.0]])


def test_rejects_diagonal_different_from_one():
    with pytest.raises(ValueError, match="diagonal"):
        _basket_spec([[0.9, 0.4], [0.4, 1.0]])


def test_rejects_non_positive_definite_correlation():
    # rho=1.5 no es una correlacion valida -- Cholesky de [[1,1.5],[1.5,1]] falla.
    with pytest.raises(ValueError, match="semidefinida positiva"):
        _basket_spec([[1.0, 1.5], [1.5, 1.0]])


def _basket_call_contract(strike, maturity=1.0):
    total = q.fixing("EQ.SPOT.A", maturity) + q.fixing("EQ.SPOT.B", maturity)
    return q.when(maturity, q.cashflow("USD", q.maximum(total - strike, 0.0)))


def _market_pricing_execution(n_paths=100_000, seed=11):
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.03], hazard_rate=0.0, recovery_rate=0.0)
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": float(n_paths), "n_steps": 4.0, "seed": float(seed)})
    execution = engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})
    return market, pricing, execution


def _price_basket(eng, model, contract, product_id, n_paths=100_000, seed=11):
    product = eng.create_product("Payoff", q.PayoffProduct(id=product_id, contract=contract).to_params())
    market, pricing, execution = _market_pricing_execution(n_paths=n_paths, seed=seed)
    return eng.price(product, ["PayoffPriceQ"], model, market, pricing, execution)["PayoffPriceQ"].scalar


def test_basket_call_prices_via_payoff_price_q_and_is_finite_and_positive():
    eng = engine.Engine()
    spec = _basket_spec([[1.0, 0.4], [0.4, 1.0]])
    model = eng.create_model("GbmBasket", spec.to_params())
    contract = _basket_call_contract(strike=180.0)

    price = _price_basket(eng, model, contract, "BASKET_CALL")
    assert price > 0.0
    assert price == price  # nunca NaN


def test_price_rejects_a_contract_that_references_an_observable_not_in_the_basket():
    eng = engine.Engine()
    spec = _basket_spec([[1.0, 0.4], [0.4, 1.0]])
    model = eng.create_model("GbmBasket", spec.to_params())
    total = q.fixing("EQ.SPOT.A", 1.0) + q.fixing("EQ.SPOT.NOT_IN_BASKET", 1.0)
    bad_contract = q.when(1.0, q.cashflow("USD", q.maximum(total - 180.0, 0.0)))

    with pytest.raises(Exception):
        _price_basket(eng, model, bad_contract, "BAD_BASKET_CALL")


def test_basket_call_price_increases_with_correlation():
    # Criterio de aceptacion explicito de PLAN_IMPROVE_NOTEBOOK.md Fase 3: subir la correlacion
    # entre dos activos con la misma vol sube el precio de una basket CALL sobre la suma (mas
    # correlacion => mas varianza de la suma => call mas cara).
    eng = engine.Engine()
    low_corr_model = eng.create_model("GbmBasket", _basket_spec([[1.0, 0.0], [0.0, 1.0]]).to_params())
    high_corr_model = eng.create_model("GbmBasket", _basket_spec([[1.0, 0.9], [0.9, 1.0]]).to_params())
    contract = _basket_call_contract(strike=200.0)

    low_price = _price_basket(eng, low_corr_model, contract, "BASKET_CALL_LOW", n_paths=150_000, seed=5)
    high_price = _price_basket(eng, high_corr_model, contract, "BASKET_CALL_HIGH", n_paths=150_000, seed=5)

    assert high_price > low_price


def test_spread_option_price_decreases_with_correlation():
    # Contraste de signo con la basket call (mismo criterio explicito del plan: "razona el signo
    # correcto para cada producto, no asumas que la correlacion siempre sube todo"): una call
    # sobre el SPREAD (S_A - S_B) tiene MENOS varianza cuanta mas correlacion hay entre A y B
    # (Var(A-B) = var_a + var_b - 2*rho*cov), asi que su valor de opcionalidad baja con rho.
    eng = engine.Engine()
    low_corr_model = eng.create_model("GbmBasket", _basket_spec([[1.0, 0.0], [0.0, 1.0]]).to_params())
    high_corr_model = eng.create_model("GbmBasket", _basket_spec([[1.0, 0.9], [0.9, 1.0]]).to_params())

    spread = q.fixing("EQ.SPOT.A", 1.0) - q.fixing("EQ.SPOT.B", 1.0)
    spread_contract = q.when(1.0, q.cashflow("USD", q.maximum(spread - 0.0, 0.0)))

    low_price = _price_basket(eng, low_corr_model, spread_contract, "SPREAD_LOW", n_paths=150_000, seed=9)
    high_price = _price_basket(eng, high_corr_model, spread_contract, "SPREAD_HIGH", n_paths=150_000, seed=9)

    assert high_price < low_price
