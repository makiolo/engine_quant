"""Tests de `engine_typed.payoff` (PLAN_PRODUCTS.md §7.3, adelantado de Fase 10): azúcar de
operadores, `to_params()`, y round-trip contra el `Engine` real -- mismo caso base que
`test_engine_typed_trade.py`.
"""

import json
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402
import engine_typed as q  # noqa: E402


def _call_100():
    return q.when(
        1.0,
        q.cashflow("USD", 1_000 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100, 0)),
    )


def test_operator_sugar_builds_expected_tree():
    call = _call_100()
    assert call.type == "when"
    amount = call.child.amount
    assert amount.type == "mul"
    assert amount.left.type == "constant" and amount.left.value == 1000.0
    assert amount.right.type == "max"
    intrinsic = amount.right
    assert intrinsic.left.type == "sub"
    assert intrinsic.left.left.type == "fixing"
    assert intrinsic.left.left.observable == "EQ.SPOT.AAPL"
    assert intrinsic.left.right.type == "constant" and intrinsic.left.right.value == 100.0
    assert intrinsic.right.type == "constant" and intrinsic.right.value == 0.0


def test_payoff_product_to_params_serializes_canonical_envelope():
    trade = q.PayoffProduct(id="AAPL_CALL_100", contract=_call_100())
    params = trade.to_params()
    document = json.loads(params["spec"])
    assert document["schema"] == "engine.payoff/v1"
    assert document["id"] == "AAPL_CALL_100"
    assert document["contract"]["type"] == "when"
    assert document["contract"]["time"] == 1.0


def test_payoff_product_discount_factor_serializes_from_alias():
    df = q.discount_factor("IR.DF.EUR.OIS", 0.0, 1.0)
    dumped = df.model_dump(mode="json", by_alias=True)
    assert dumped["from"] == 0.0
    assert "from_" not in dumped


def test_payoff_product_creates_real_engine_product():
    eng = engine.Engine()
    trade = q.PayoffProduct(id="AAPL_CALL_100", contract=_call_100())
    product = eng.create_product(trade.product_type, trade.to_params())
    assert product.type_name == "Payoff"


def test_payoff_product_rejects_invalid_contract():
    eng = engine.Engine()
    # Cashflow sin When/Trigger envolvente: ValidationVisitor lo rechaza (ADR-P0-08), el error
    # agregado de PayoffProduct::build_program se propaga como excepcion de C++ hacia Python.
    invalid = q.PayoffProduct(id="BAD", contract=q.cashflow("USD", q.constant(1.0)))
    try:
        eng.create_product(invalid.product_type, invalid.to_params())
        assert False, "se esperaba una excepcion (Cashflow sin instante activo)"
    except Exception as e:  # noqa: BLE001 -- el tipo exacto de excepcion cruzando nanobind no esta documentado aqui
        assert "instante activo" in str(e)


if __name__ == "__main__":
    test_operator_sugar_builds_expected_tree()
    test_payoff_product_to_params_serializes_canonical_envelope()
    test_payoff_product_discount_factor_serializes_from_alias()
    test_payoff_product_creates_real_engine_product()
    test_payoff_product_rejects_invalid_contract()
    print("OK: tests de engine_typed.payoff pasaron")
