"""Tests de `engine.Portfolio` (PLAN_BACKWARD.md §6.4/§9 Fase 6): objeto first-class de
orquestacion (lista de trades bajo un UNICO IModel/MarketSnapshot). Mismo criterio que
test_engine_typed_greeks.py -- no repite la verificacion numerica fina de compute_hessian/
compute_hvp (ya cubierta exhaustivamente por PortfolioTest en cpp/engine/tests/
test_portfolio.cpp), aqui se confirma que el binding nanobind expone la misma
semantica/identidad exacta que la capa C++, y que el ownership de Product sobrevive a pasar por
Engine.price(...) Y Portfolio.add(...) a la vez.
"""

import math
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402


def _hull_white_params():
    return {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02}


def _irs_params(notional, fixed_rate):
    return {
        "notional": notional,
        "fixed_rate": fixed_rate,
        "payment_times": [1.0, 2.0, 3.0],
        "accruals": [1.0, 1.0, 1.0],
    }


def _hull_white_portfolio_fixture():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    trade_a = eng.create_product("IRSwap", _irs_params(1_000_000.0, 0.02))
    trade_b = eng.create_product("IRSwap", _irs_params(2_000_000.0, 0.025))
    trade_c = eng.create_product("IRSwap", _irs_params(500_000.0, 0.018))
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 1000.0, "n_steps": 1.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu"})
    return eng, [trade_a, trade_b, trade_c], model, market, pricing, execution


def test_portfolio_starts_empty_and_size_grows_with_add():
    portfolio = engine.Portfolio()
    assert portfolio.size() == 0
    assert len(portfolio) == 0
    assert portfolio.trades == []

    eng = engine.Engine()
    trade = eng.create_product("IRSwap", _irs_params(1_000_000.0, 0.02))
    portfolio.add(trade)
    assert portfolio.size() == 1
    assert len(portfolio) == 1
    assert len(portfolio.trades) == 1


def test_portfolio_add_does_not_steal_ownership_from_engine_price():
    # PLAN_BACKWARD.md §9 Fase 6: el mismo objeto Product ya usado en Engine.price(...) se pasa
    # tambien a Portfolio.add(...) -- ambos deben seguir funcionando despues (verifica que
    # nanobind construye el shared_ptr<const IProduct> de Portfolio::add sin robarle la
    # propiedad al objeto Python "Product", sea cual sea el holder con el que nanobind ya lo
    # gestionaba internamente).
    eng, trades, model, market, pricing, execution = _hull_white_portfolio_fixture()
    trade = trades[0]

    result_before = eng.price(trade, ["PV"], model, market, pricing, execution)
    pv_before = result_before["PV"].scalar

    portfolio = engine.Portfolio()
    portfolio.add(trade)
    assert portfolio.size() == 1

    # `trade` se sigue pudiendo usar directamente para pricing individual despues de anadirlo.
    result_after = eng.price(trade, ["PV"], model, market, pricing, execution)
    assert math.isclose(result_after["PV"].scalar, pv_before, rel_tol=0.0, abs_tol=1e-9)

    # Y el portfolio en si funciona con ese mismo trade compartido.
    batch = portfolio.price(["PV"], model, market, pricing, execution)
    assert len(batch) == 1
    assert math.isclose(batch[0].measures["PV"].scalar, pv_before, rel_tol=0.0, abs_tol=1e-9)


def test_portfolio_price_matches_price_many_on_the_same_trades():
    eng, trades, model, market, pricing, execution = _hull_white_portfolio_fixture()

    portfolio = engine.Portfolio()
    for trade in trades:
        portfolio.add(trade)
    assert portfolio.size() == 3

    from_portfolio = portfolio.price(["PV", "DV01"], model, market, pricing, execution)
    from_price_many = eng.price_many(trades, ["PV", "DV01"], model, market, pricing, execution)

    assert len(from_portfolio) == len(from_price_many)
    for a, b in zip(from_portfolio, from_price_many):
        assert a.trade_index == b.trade_index
        for name in ("PV", "DV01"):
            assert math.isclose(a.measures[name].scalar, b.measures[name].scalar, rel_tol=0.0, abs_tol=1e-9)


def test_portfolio_hessian_matches_manual_sum_of_per_trade_hessian_exactly():
    eng, trades, model, market, pricing, execution = _hull_white_portfolio_fixture()

    portfolio = engine.Portfolio()
    for trade in trades:
        portfolio.add(trade)

    portfolio_report = portfolio.hessian("HullWhiteModelNpv", model, market, pricing, execution)
    assert portfolio_report.skipped == []
    assert len(portfolio_report.entries) == 10

    manual_per_trade = [eng.hessian(trade, "HullWhiteModelNpv", model, market, pricing, execution) for trade in trades]
    for report in manual_per_trade:
        assert report.skipped == []
        assert len(report.entries) == 10

    def manual_sum(factor_i, factor_j):
        total = 0.0
        for report in manual_per_trade:
            found = None
            for e in report.entries:
                if (e.factor_i == factor_i and e.factor_j == factor_j) or (e.factor_i == factor_j and e.factor_j == factor_i):
                    found = e
                    break
            assert found is not None, f"par no encontrado en un trade individual: {factor_i}/{factor_j}"
            total += found.value
        return total

    for entry in portfolio_report.entries:
        expected = manual_sum(entry.factor_i, entry.factor_j)
        assert math.isclose(entry.value, expected, rel_tol=0.0, abs_tol=1e-9 * max(1.0, abs(expected)))
        assert entry.method_used == "aad_forward_over_forward"
        # Hull-White (formula cerrada) nunca lleva std_error.
        assert entry.std_error is None


def test_portfolio_hvp_matches_manual_sum_of_per_trade_hvp_exactly():
    eng, trades, model, market, pricing, execution = _hull_white_portfolio_fixture()

    portfolio = engine.Portfolio()
    for trade in trades:
        portfolio.add(trade)

    direction = {"model.a": 1.0, "model.b": 0.5, "model.sigma": -0.25, "model.r0": 2.0}
    portfolio_hvp = portfolio.hvp("HullWhiteModelNpv", model, market, pricing, execution, direction=direction)
    assert portfolio_hvp.skipped == []
    assert len(portfolio_hvp.components) == 4

    manual_per_trade = [
        eng.hvp(trade, "HullWhiteModelNpv", model, market, pricing, execution, direction=direction) for trade in trades
    ]

    def manual_sum(factor):
        total = 0.0
        for report in manual_per_trade:
            found = next((c for c in report.components if c.factor == factor), None)
            assert found is not None, f"factor no encontrado en un trade individual: {factor}"
            total += found.value
        return total

    for component in portfolio_hvp.components:
        expected = manual_sum(component.factor)
        assert math.isclose(component.value, expected, rel_tol=0.0, abs_tol=1e-9 * max(1.0, abs(expected)))


def test_portfolio_price_on_empty_portfolio_raises_like_price_many():
    # price() es un envoltorio fino sobre price_many YA EXISTENTE: hereda su comportamiento tal
    # cual, incluido que una lista de trades vacia es un error explicito.
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", _hull_white_params())
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 1000.0, "n_steps": 1.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu"})

    portfolio = engine.Portfolio()
    try:
        portfolio.price(["PV"], model, market, pricing, execution)
    except Exception:
        return
    raise AssertionError("se esperaba una excepcion al pedir price() sobre un Portfolio vacio")


if __name__ == "__main__":
    test_portfolio_starts_empty_and_size_grows_with_add()
    test_portfolio_add_does_not_steal_ownership_from_engine_price()
    test_portfolio_price_matches_price_many_on_the_same_trades()
    test_portfolio_hessian_matches_manual_sum_of_per_trade_hessian_exactly()
    test_portfolio_hvp_matches_manual_sum_of_per_trade_hvp_exactly()
    test_portfolio_price_on_empty_portfolio_raises_like_price_many()
    print("OK: tests de engine.Portfolio pasaron")
