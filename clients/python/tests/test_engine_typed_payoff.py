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


def test_european_call_template_matches_manual_ast():
    templated = q.european_call("AAPL_CALL_100", "EQ.SPOT.AAPL", 100.0, 1_000.0, 1.0)
    manual = q.PayoffProduct(id="AAPL_CALL_100", contract=_call_100())
    assert templated.contract == manual.contract


def test_european_put_template_builds_expected_tree():
    put = q.european_put("AAPL_PUT_100", "EQ.SPOT.AAPL", 100.0, 1_000.0, 1.0)
    assert put.contract.type == "when"
    amount = put.contract.child.amount
    assert amount.type == "mul"
    intrinsic = amount.right
    assert intrinsic.type == "max"
    assert intrinsic.left.left.type == "constant" and intrinsic.left.left.value == 100.0
    assert intrinsic.left.right.type == "fixing"


def test_irs_template_replicates_irs_swap_cpp_template():
    trade = q.irs("SWAP_2Y_3PCT", notional=1_000_000.0, fixed_rate=0.03, payment_times=[1.0, 2.0], accruals=[1.0, 1.0])
    assert trade.contract.type == "both"
    floating_leg, given_fixed_leg = trade.contract.children
    assert floating_leg.type == "both"
    start_flow, last_flow = floating_leg.children
    assert start_flow.child.amount.value == 1_000_000.0
    assert last_flow.child.amount.value == -1_000_000.0
    assert given_fixed_leg.type == "give"
    fixed_leg = given_fixed_leg.child
    assert fixed_leg.type == "both"
    assert len(fixed_leg.children) == 2
    assert fixed_leg.children[0].child.amount.value == 1_000_000.0 * 0.03 * 1.0


def test_irs_template_rejects_mismatched_schedule_lengths():
    try:
        q.irs("BAD", notional=1.0, fixed_rate=0.03, payment_times=[1.0, 2.0], accruals=[1.0])
        assert False, "se esperaba ValueError"
    except ValueError as e:
        assert "igual longitud" in str(e)


def test_fx_forward_template_matches_plan_ast():
    trade = q.fx_forward("EURUSD_FWD", "EUR", "USD", 1_000_000.0, 1.10, 1.0)
    assert trade.contract.type == "when"
    foreign_flow, domestic_flow = trade.contract.child.children
    assert foreign_flow.currency == "EUR" and foreign_flow.amount.value == 1_000_000.0
    assert domestic_flow.currency == "USD" and domestic_flow.amount.value == -1_100_000.0


def test_fx_forward_template_sign_flips_both_legs():
    trade = q.fx_forward("EURUSD_FWD_SELL", "EUR", "USD", 1_000_000.0, 1.10, 1.0, sign=-1.0)
    foreign_flow, domestic_flow = trade.contract.child.children
    assert foreign_flow.amount.value == -1_000_000.0
    assert domestic_flow.amount.value == 1_100_000.0


if __name__ == "__main__":
    test_operator_sugar_builds_expected_tree()
    test_payoff_product_to_params_serializes_canonical_envelope()
    test_payoff_product_discount_factor_serializes_from_alias()
    test_payoff_product_creates_real_engine_product()
    test_payoff_product_rejects_invalid_contract()
    test_european_call_template_matches_manual_ast()
    test_european_put_template_builds_expected_tree()
    test_irs_template_replicates_irs_swap_cpp_template()
    test_irs_template_rejects_mismatched_schedule_lengths()
    test_fx_forward_template_matches_plan_ast()
    test_fx_forward_template_sign_flips_both_legs()
    print("OK: tests de engine_typed.payoff pasaron")
