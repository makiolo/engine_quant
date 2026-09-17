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


def test_product_explain_includes_id_hash_and_tree():
    eng = engine.Engine()
    trade = q.PayoffProduct(id="AAPL_CALL_100", contract=_call_100())
    product = eng.create_product(trade.product_type, trade.to_params())
    text = product.explain()
    assert "AAPL_CALL_100" in text
    assert "When" in text


def test_legacy_product_explain_defaults_to_type_name():
    eng = engine.Engine()
    irs = eng.create_product(
        "IRSwap",
        {"notional": 1_000_000.0, "payment_times": [1.0], "accruals": [1.0], "fixed_rate": 0.03},
    )
    assert irs.explain() == "IRSwap"


def test_validate_payoff_spec_returns_empty_list_for_valid_spec():
    trade = q.PayoffProduct(id="AAPL_CALL_100", contract=_call_100())
    errors = engine.validate_payoff_spec(trade.to_params()["spec"])
    assert errors == []


def test_validate_payoff_spec_reports_errors_without_creating_product():
    invalid = q.PayoffProduct(id="BAD", contract=q.cashflow("USD", q.constant(1.0)))
    errors = engine.validate_payoff_spec(invalid.to_params()["spec"])
    assert len(errors) == 1
    assert "instante activo" in errors[0]


# -------------------------------------------------------------------------------------------
# PLAN_IMPROVE_NOTEBOOK.md Fase 2: q.average/q.running_min/q.running_max ya existian como
# builders de engine_typed.payoff (misma forma que el AST de autoria C++ y
# docs/schema/engine.payoff/v1.schema.json: 'average' es una suma PONDERADA schedule+weights, no
# una media con divisor implicito; 'running_min'/'running_max' solo llevan 'observable', sin
# schedule propio -- reducen sobre el instante activo, ver ir.rs::ScalarOp::RunningMin) pero el
# compilador Monte Carlo Rust los rechazaba en preflight. Paridad EXACTA (misma seed/n_paths,
# nunca solo "dentro de ruido Monte Carlo") entre el nodo nativo y la replica horneada a mano con
# los nodos ya soportados -- mismo criterio que los tests de paridad de eval.rs en Rust.
# -------------------------------------------------------------------------------------------

_LOOKBACK_MODEL = {"s0": 100.0, "r": 0.03, "q": 0.0, "sigma": 0.25, "observable": "EQ.SPOT.AAPL"}
_LOOKBACK_SCHEDULE = [0.25, 0.5, 0.75, 1.0]


def _lookback_market_pricing_execution():
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.03], hazard_rate=0.0, recovery_rate=0.0)
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 20_000.0, "n_steps": 4.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})
    return market, pricing, execution


def _price(eng, model, contract, product_id):
    product = eng.create_product("Payoff", q.PayoffProduct(id=product_id, contract=contract).to_params())
    market, pricing, execution = _lookback_market_pricing_execution()
    return eng.price(product, ["PayoffPriceQ"], model, market, pricing, execution)["PayoffPriceQ"].scalar


def test_average_native_matches_hand_baked_fixings_sum_exactly():
    eng = engine.Engine()
    model = eng.create_model("GBM", _LOOKBACK_MODEL)
    weights = [0.25, 0.25, 0.25, 0.25]

    native_average = q.average("EQ.SPOT.AAPL", _LOOKBACK_SCHEDULE, weights)
    native_contract = q.when(1.0, q.cashflow("USD", q.maximum(native_average - 100.0, 0.0)))

    manual_average = q.fixing("EQ.SPOT.AAPL", _LOOKBACK_SCHEDULE[0]) * weights[0]
    for t, w in zip(_LOOKBACK_SCHEDULE[1:], weights[1:]):
        manual_average = manual_average + q.fixing("EQ.SPOT.AAPL", t) * w
    manual_contract = q.when(1.0, q.cashflow("USD", q.maximum(manual_average - 100.0, 0.0)))

    native_price = _price(eng, model, native_contract, "ASIAN_NATIVE")
    manual_price = _price(eng, model, manual_contract, "ASIAN_MANUAL")
    assert native_price == manual_price


def test_running_max_native_matches_hand_chained_maximum_exactly():
    # RunningMax no lleva schedule propio: el patron "ancla" (un 'average' con weights a cero,
    # documentado en ir.rs::ScalarOp::RunningMin) inyecta el schedule de monitorizacion en
    # required_times() sin contribuir ningun importe -- mismo patron que usa el notebook 02.
    eng = engine.Engine()
    model = eng.create_model("GBM", _LOOKBACK_MODEL)
    anchor = q.average("EQ.SPOT.AAPL", _LOOKBACK_SCHEDULE, [0.0] * len(_LOOKBACK_SCHEDULE))
    native_amount = q.maximum(q.running_max("EQ.SPOT.AAPL") + 0.0 * anchor - 100.0, 0.0)
    native_contract = q.when(1.0, q.cashflow("USD", native_amount))

    manual_running_max = q.fixing("EQ.SPOT.AAPL", _LOOKBACK_SCHEDULE[0])
    for t in _LOOKBACK_SCHEDULE[1:]:
        manual_running_max = q.maximum(manual_running_max, q.fixing("EQ.SPOT.AAPL", t))
    manual_contract = q.when(1.0, q.cashflow("USD", q.maximum(manual_running_max - 100.0, 0.0)))

    native_price = _price(eng, model, native_contract, "LOOKBACK_CALL_NATIVE")
    manual_price = _price(eng, model, manual_contract, "LOOKBACK_CALL_MANUAL")
    assert native_price == manual_price


def test_running_min_native_matches_hand_chained_minimum_exactly():
    eng = engine.Engine()
    model = eng.create_model("GBM", _LOOKBACK_MODEL)
    anchor = q.average("EQ.SPOT.AAPL", _LOOKBACK_SCHEDULE, [0.0] * len(_LOOKBACK_SCHEDULE))
    native_amount = q.maximum(100.0 - (q.running_min("EQ.SPOT.AAPL") + 0.0 * anchor), 0.0)
    native_contract = q.when(1.0, q.cashflow("USD", native_amount))

    manual_running_min = q.fixing("EQ.SPOT.AAPL", _LOOKBACK_SCHEDULE[0])
    for t in _LOOKBACK_SCHEDULE[1:]:
        manual_running_min = q.minimum(manual_running_min, q.fixing("EQ.SPOT.AAPL", t))
    manual_contract = q.when(1.0, q.cashflow("USD", q.maximum(100.0 - manual_running_min, 0.0)))

    native_price = _price(eng, model, native_contract, "LOOKBACK_PUT_NATIVE")
    manual_price = _price(eng, model, manual_contract, "LOOKBACK_PUT_MANUAL")
    assert native_price == manual_price


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
    test_product_explain_includes_id_hash_and_tree()
    test_legacy_product_explain_defaults_to_type_name()
    test_validate_payoff_spec_returns_empty_list_for_valid_spec()
    test_validate_payoff_spec_reports_errors_without_creating_product()
    test_average_native_matches_hand_baked_fixings_sum_exactly()
    test_running_max_native_matches_hand_chained_maximum_exactly()
    test_running_min_native_matches_hand_chained_minimum_exactly()
    print("OK: tests de engine_typed.payoff pasaron")
