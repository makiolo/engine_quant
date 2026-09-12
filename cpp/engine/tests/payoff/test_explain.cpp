// Smoke test de `ExplainVisitor` (PLAN_PRODUCTS.md §5.1, §12 Fase 1): confirma que el árbol y
// el ledger se explican de forma legible, no la exactitud de cada línea de formato.

#include <gtest/gtest.h>

#include "engine/payoff/explain_visitor.hpp"
#include "engine/payoff/scenario_evaluator.hpp"

namespace {

using namespace engine::payoff;

TEST(ExplainVisitorTest, ExplainTreeMentionsEachNodeKind) {
    ContractPtr call = when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0))
        )
    );
    ExplainVisitor explainer;
    std::string text = explainer.explain_tree(call);

    EXPECT_NE(text.find("When"), std::string::npos);
    EXPECT_NE(text.find("Cashflow"), std::string::npos);
    EXPECT_NE(text.find("Max"), std::string::npos);
    EXPECT_NE(text.find("Fixing"), std::string::npos);
    EXPECT_NE(text.find("EQ.SPOT.AAPL"), std::string::npos);
}

TEST(ExplainVisitorTest, ExplainLedgerMentionsTimeCurrencyAndAmount) {
    ContractPtr call = when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            mul(constant(1000.0), maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0)))
        )
    );
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    CashflowLedger ledger = evaluator.evaluate(call, context);

    ExplainVisitor explainer;
    std::string text = explainer.explain_ledger(ledger);
    EXPECT_NE(text.find("USD"), std::string::npos);
    EXPECT_NE(text.find("30000"), std::string::npos);
}

} // namespace
