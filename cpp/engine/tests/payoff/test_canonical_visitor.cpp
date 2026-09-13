// Tests de `CanonicalVisitor` (PLAN_PRODUCTS.md §7.2, §12 Fase 3): formato exacto, hash
// estable/distinto, y el round-trip AST -> JSON -> AST' (§14 "hash y version del payoff").

#include <gtest/gtest.h>

#include <limits>

#include "engine/payoff/canonical_visitor.hpp"
#include "engine/payoff/json_parser.hpp"
#include "engine/payoff/scenario_evaluator.hpp"

namespace {

using namespace engine::payoff;

ContractPtr call_100() {
    return when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            mul(constant(1000.0), maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0)))
        )
    );
}

TEST(CanonicalVisitorTest, ToJsonProducesExactExpectedText) {
    ContractPtr tree = when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(30000.0)));
    std::string json = CanonicalVisitor::to_json("SIMPLE", tree);
    EXPECT_EQ(
        json,
        R"({"schema":"engine.payoff/v1","id":"SIMPLE","contract":{"type":"when","time":1,"child":{"type":"cashflow","currency":"USD","amount":{"type":"constant","value":30000}}}})"
    );
}

TEST(CanonicalVisitorTest, HashIsStableAcrossRepeatedCalls) {
    ContractPtr tree = call_100();
    std::string h1 = CanonicalVisitor::hash("AAPL_CALL_100", tree);
    std::string h2 = CanonicalVisitor::hash("AAPL_CALL_100", tree);
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 16u);
}

TEST(CanonicalVisitorTest, HashDiffersForDifferentAst) {
    std::string h1 = CanonicalVisitor::hash("X", when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(1.0))));
    std::string h2 = CanonicalVisitor::hash("X", when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(2.0))));
    EXPECT_NE(h1, h2);
}

TEST(CanonicalVisitorTest, HashDiffersForDifferentId) {
    ContractPtr tree = call_100();
    EXPECT_NE(CanonicalVisitor::hash("A", tree), CanonicalVisitor::hash("B", tree));
}

TEST(CanonicalVisitorTest, NegativeZeroNormalizesSameAsPositiveZero) {
    std::string h1 = CanonicalVisitor::hash("X", when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(0.0))));
    std::string h2 = CanonicalVisitor::hash("X", when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(-0.0))));
    EXPECT_EQ(h1, h2);
}

TEST(CanonicalVisitorTest, RoundTripAstToJsonToAstPreservesHashAndLedger) {
    ContractPtr original = call_100();
    std::string json = CanonicalVisitor::to_json("AAPL_CALL_100", original);

    ParsedPayoffDocument doc = parse_payoff_document(json);
    EXPECT_EQ(doc.id, "AAPL_CALL_100");

    std::string hash_original = CanonicalVisitor::hash("AAPL_CALL_100", original);
    std::string hash_roundtrip = CanonicalVisitor::hash(doc.id, doc.contract);
    EXPECT_EQ(hash_original, hash_roundtrip);

    MarketPath path1;
    path1.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);
    FixingStore historical1;
    RuntimeState state1;
    EvaluationContext context1{path1, historical1, state1};
    CashflowLedger ledger_original = ScenarioEvaluator{}.evaluate(original, context1);

    MarketPath path2;
    path2.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);
    FixingStore historical2;
    RuntimeState state2;
    EvaluationContext context2{path2, historical2, state2};
    CashflowLedger ledger_roundtrip = ScenarioEvaluator{}.evaluate(doc.contract, context2);

    ASSERT_EQ(ledger_original.size(), ledger_roundtrip.size());
    ASSERT_EQ(ledger_original.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger_original[0].amount, ledger_roundtrip[0].amount);
    EXPECT_DOUBLE_EQ(ledger_original[0].payment_time.year_fraction, ledger_roundtrip[0].payment_time.year_fraction);
    EXPECT_EQ(ledger_original[0].currency, ledger_roundtrip[0].currency);
}

TEST(CanonicalVisitorTest, RejectsNonFiniteConstantAtSerialization) {
    ContractPtr invalid = when(
        TimePoint{1.0}, cashflow(Currency{"USD"}, constant(std::numeric_limits<double>::infinity()))
    );
    EXPECT_THROW(CanonicalVisitor::to_json("X", invalid), ValidationError);
}

} // namespace
