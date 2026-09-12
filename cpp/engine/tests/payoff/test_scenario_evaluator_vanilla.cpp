// Tests de `ScenarioEvaluator` (PLAN_PRODUCTS.md §12 Fase 1): fixtures de Fase 0
// (call/put/forward/swap fijo) contra ledgers manuales, más las propiedades algebraicas de
// §13.2 diferidas desde test_contract_basic.cpp (dependen del evaluador, no solo del AST).

#include <gtest/gtest.h>

#include <string>

#include "engine/payoff/scenario_evaluator.hpp"

namespace {

using namespace engine::payoff;

MarketPath market_with_aapl_fixing(double spot_at_1y) {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, spot_at_1y);
    return path;
}

CashflowLedger evaluate(const ContractPtr& contract, MarketPath& path) {
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    return evaluator.evaluate(contract, context);
}

ContractPtr call_100() {
    return when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            mul(constant(1000.0), maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0)))
        )
    );
}

ContractPtr put_100() {
    return when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            mul(constant(1000.0), maximum(sub(constant(100.0), fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0})), constant(0.0)))
        )
    );
}

ContractPtr forward_100() {
    return when(
        TimePoint{1.0},
        cashflow(Currency{"USD"}, mul(constant(1000.0), sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0))))
    );
}

ContractPtr fixed_leg_swap() {
    return both({
        when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(30000.0))),
        when(TimePoint{2.0}, cashflow(Currency{"USD"}, constant(30000.0))),
    });
}

double total_amount(const CashflowLedger& ledger) {
    double total = 0.0;
    for (const auto& entry : ledger) total += entry.amount;
    return total;
}

TEST(ScenarioEvaluatorTest, CallPaysIntrinsicValueWhenInTheMoney) {
    MarketPath path = market_with_aapl_fixing(130.0);
    CashflowLedger ledger = evaluate(call_100(), path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 1.0);
    EXPECT_EQ(ledger[0].currency, Currency{"USD"});
    EXPECT_DOUBLE_EQ(ledger[0].amount, 30000.0);
}

TEST(ScenarioEvaluatorTest, PutCallParityHoldsOnLedger) {
    MarketPath path1 = market_with_aapl_fixing(130.0);
    CashflowLedger call_ledger = evaluate(call_100(), path1);
    MarketPath path2 = market_with_aapl_fixing(130.0);
    CashflowLedger put_ledger = evaluate(put_100(), path2);
    ASSERT_EQ(call_ledger.size(), 1u);
    ASSERT_EQ(put_ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(call_ledger[0].amount - put_ledger[0].amount, 1000.0 * (130.0 - 100.0));
}

TEST(ScenarioEvaluatorTest, ForwardPaysLinearPayoffEvenWhenNegative) {
    MarketPath path1 = market_with_aapl_fixing(80.0);
    CashflowLedger forward_ledger = evaluate(forward_100(), path1);
    MarketPath path2 = market_with_aapl_fixing(80.0);
    CashflowLedger call_ledger = evaluate(call_100(), path2);
    ASSERT_EQ(forward_ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(forward_ledger[0].amount, 1000.0 * (80.0 - 100.0));
    EXPECT_DOUBLE_EQ(call_ledger[0].amount, 0.0);
}

TEST(ScenarioEvaluatorTest, FixedLegSwapPaysBothCoupons) {
    MarketPath path;
    CashflowLedger ledger = evaluate(fixed_leg_swap(), path);
    ASSERT_EQ(ledger.size(), 2u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 1.0);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 30000.0);
    EXPECT_DOUBLE_EQ(ledger[1].payment_time.year_fraction, 2.0);
    EXPECT_DOUBLE_EQ(ledger[1].amount, 30000.0);
}

TEST(ScenarioEvaluatorTest, MissingFixingThrowsEvaluationErrorMentioningObservable) {
    MarketPath empty_path;
    try {
        evaluate(call_100(), empty_path);
        FAIL() << "se esperaba EvaluationError";
    } catch (const EvaluationError& e) {
        EXPECT_NE(std::string(e.what()).find("EQ.SPOT.AAPL"), std::string::npos);
    }
}

TEST(ScenarioEvaluatorTest, DivisionByZeroThrowsEvaluationError) {
    ContractPtr tree = when(TimePoint{1.0}, cashflow(Currency{"USD"}, div(constant(1.0), constant(0.0))));
    MarketPath path;
    EXPECT_THROW(evaluate(tree, path), EvaluationError);
}

// ---- Propiedades algebraicas (§13.2), diferidas desde test_contract_basic.cpp: dependen del
// evaluador, no solo de la forma del AST (ver comentario en ese archivo). ----

TEST(ScenarioEvaluatorProperties, BothWithZeroEqualsChild) {
    MarketPath path1 = market_with_aapl_fixing(130.0);
    CashflowLedger plain = evaluate(call_100(), path1);
    MarketPath path2 = market_with_aapl_fixing(130.0);
    CashflowLedger with_zero = evaluate(both({call_100(), zero()}), path2);
    EXPECT_DOUBLE_EQ(total_amount(plain), total_amount(with_zero));
}

TEST(ScenarioEvaluatorProperties, GiveOfGiveEqualsOriginal) {
    MarketPath path1 = market_with_aapl_fixing(130.0);
    CashflowLedger plain = evaluate(call_100(), path1);
    MarketPath path2 = market_with_aapl_fixing(130.0);
    CashflowLedger double_give = evaluate(give(give(call_100())), path2);
    EXPECT_DOUBLE_EQ(total_amount(plain), total_amount(double_give));
}

TEST(ScenarioEvaluatorProperties, ScaleByZeroIsZero) {
    MarketPath path = market_with_aapl_fixing(130.0);
    CashflowLedger scaled = evaluate(scale(constant(0.0), call_100()), path);
    EXPECT_DOUBLE_EQ(total_amount(scaled), 0.0);
}

TEST(ScenarioEvaluatorProperties, PermutingBothChildrenDoesNotChangeAggregateLedger) {
    MarketPath path1 = market_with_aapl_fixing(130.0);
    CashflowLedger ledger_a = evaluate(both({call_100(), fixed_leg_swap()}), path1);
    MarketPath path2 = market_with_aapl_fixing(130.0);
    CashflowLedger ledger_b = evaluate(both({fixed_leg_swap(), call_100()}), path2);
    EXPECT_DOUBLE_EQ(total_amount(ledger_a), total_amount(ledger_b));
}

TEST(ScenarioEvaluatorProperties, ScaleByMinusOneEqualsGive) {
    MarketPath path1 = market_with_aapl_fixing(130.0);
    CashflowLedger scaled = evaluate(scale(constant(-1.0), call_100()), path1);
    MarketPath path2 = market_with_aapl_fixing(130.0);
    CashflowLedger given = evaluate(give(call_100()), path2);
    EXPECT_DOUBLE_EQ(total_amount(scaled), total_amount(given));
}

} // namespace
