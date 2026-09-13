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

TEST(ExplainVisitorTest, ExplainTreeShowsExerciseStructureDatesAndValueExpression) {
    // PLAN_PRODUCTS.md §10, Fase 9, criterio de aceptacion: "explain muestra la politica" -- a
    // nivel de estructura del AST (fechas y como se calcula el ejercicio), no de la decision de
    // Longstaff-Schwartz resuelta por el pricer (eso es ExercisePolicyResult, ver measures.hpp).
    ContractPtr bermuda_put = exercise(
        EventId{"EX"}, {TimePoint{0.5}, TimePoint{1.0}},
        maximum(sub(constant(100.0), current(ObservableId{"EQ.SPOT.AAPL"})), constant(0.0)), zero()
    );
    ExplainVisitor explainer;
    std::string text = explainer.explain_tree(bermuda_put);

    EXPECT_NE(text.find("Exercise(EX"), std::string::npos);
    EXPECT_NE(text.find("0.500000"), std::string::npos);
    EXPECT_NE(text.find("1.000000"), std::string::npos);
    EXPECT_NE(text.find("Max"), std::string::npos);
    EXPECT_NE(text.find("Current(EQ.SPOT.AAPL)"), std::string::npos);
    EXPECT_NE(text.find("continuation"), std::string::npos);
}

} // namespace
