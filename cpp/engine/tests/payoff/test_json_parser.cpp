// Tests de `parse_payoff_document` (PLAN_PRODUCTS.md §7.2, §12 Fase 3): JSON literal inline
// (no se leen docs/schema/engine.payoff/examples/*.json desde C++ -- eso ya lo cubre
// test_payoff_schema.py) cubriendo representantes de las tres jerarquias y los casos de
// rechazo (schema, campo desconocido, JSON malformado, limites de recursos).

#include <gtest/gtest.h>

#include "engine/payoff/json_parser.hpp"
#include "engine/payoff/scenario_evaluator.hpp"

namespace {

using namespace engine::payoff;

TEST(JsonParserTest, ParsesCallFixtureAndEvaluatesToExpectedLedger) {
    const char* json = R"({
        "schema": "engine.payoff/v1",
        "id": "AAPL_CALL_100",
        "contract": {
            "type": "when",
            "time": 1.0,
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "mul",
                    "left": {"type": "constant", "value": 1000.0},
                    "right": {
                        "type": "max",
                        "left": {
                            "type": "sub",
                            "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
                            "right": {"type": "constant", "value": 100.0}
                        },
                        "right": {"type": "constant", "value": 0.0}
                    }
                }
            }
        }
    })";

    ParsedPayoffDocument doc = parse_payoff_document(json);
    EXPECT_EQ(doc.id, "AAPL_CALL_100");

    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    CashflowLedger ledger = evaluator.evaluate(doc.contract, context);

    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 30000.0);
}

TEST(JsonParserTest, ParsesArithmeticAndTranscendentalScalarNodes) {
    const char* json = R"({
        "schema": "engine.payoff/v1",
        "id": "SCALAR_MIX",
        "contract": {
            "type": "when",
            "time": 1.0,
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "clamp",
                    "value": {
                        "type": "pow",
                        "base": {"type": "exp", "operand": {"type": "log", "operand": {"type": "constant", "value": 2.0}}},
                        "exponent": {"type": "constant", "value": 1.0}
                    },
                    "low": {"type": "neg", "operand": {"type": "abs", "operand": {"type": "constant", "value": -5.0}}},
                    "high": {"type": "constant", "value": 100.0}
                }
            }
        }
    })";

    ParsedPayoffDocument doc = parse_payoff_document(json);
    MarketPath path;
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    CashflowLedger ledger = evaluator.evaluate(doc.contract, context);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 2.0); // exp(log(2))^1 = 2, dentro de [-5, 100]
}

TEST(JsonParserTest, ParsesAverageDiscountFactorAndFxConversion) {
    const char* json = R"({
        "schema": "engine.payoff/v1",
        "id": "MARKET_NODES",
        "contract": {
            "type": "when",
            "time": 1.0,
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "add",
                    "left": {
                        "type": "average",
                        "observable": "EQ.SPOT.AAPL",
                        "schedule": [0.25, 0.5],
                        "weights": [0.5, 0.5]
                    },
                    "right": {
                        "type": "mul",
                        "left": {"type": "discount_factor", "curve": "IR.DF.EUR.OIS", "from": 0.0, "to": 1.0},
                        "right": {"type": "fx_conversion", "from_currency": "EUR", "to_currency": "USD", "time": 1.0}
                    }
                }
            }
        }
    })";

    ParsedPayoffDocument doc = parse_payoff_document(json);
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.25}, 100.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 110.0);
    path.set_discount_factor(CurveId{"IR.DF.EUR.OIS"}, TimePoint{0.0}, TimePoint{1.0}, 0.95);
    path.set_fx_rate(Currency{"EUR"}, Currency{"USD"}, TimePoint{1.0}, 1.1);
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    CashflowLedger ledger = evaluator.evaluate(doc.contract, context);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 105.0 + 0.95 * 1.1);
}

TEST(JsonParserTest, ParsesTriggerWithAllPredicateAndEventNodes) {
    const char* json = R"({
        "schema": "engine.payoff/v1",
        "id": "BARRIER",
        "contract": {
            "type": "trigger",
            "id": "UI",
            "monitoring_times": [0.5, 1.0],
            "condition": {
                "type": "all",
                "operands": [
                    {"type": "greater_equal", "left": {"type": "current", "observable": "EQ.SPOT.AAPL"}, "right": {"type": "constant", "value": 120.0}},
                    {"type": "not", "operand": {"type": "event_occurred", "event": "OTHER"}}
                ]
            },
            "monitoring": "discrete",
            "settlement": "at_hit",
            "priority": 0,
            "latch": true,
            "on_hit": {"type": "cashflow", "currency": "USD", "amount": {"type": "event_value", "event": "UI", "observable": "EQ.SPOT.AAPL"}},
            "on_miss": {"type": "zero"}
        }
    })";

    ParsedPayoffDocument doc = parse_payoff_document(json);
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 125.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    CashflowLedger ledger = evaluator.evaluate(doc.contract, context);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 125.0);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 0.5);
}

TEST(JsonParserTest, ParsesExerciseNodeStructurallyWithoutEvaluating) {
    const char* json = R"({
        "schema": "engine.payoff/v1",
        "id": "EXERCISE_STUB",
        "contract": {
            "type": "exercise",
            "id": "EX",
            "dates": [1.0, 2.0],
            "exercise_value": {"type": "constant", "value": 0.0},
            "continuation": {"type": "zero"}
        }
    })";
    ParsedPayoffDocument doc = parse_payoff_document(json);
    EXPECT_NE(dynamic_cast<const Exercise*>(doc.contract.get()), nullptr);
}

TEST(JsonParserTest, ParsesGiveBothScaleIf) {
    const char* json = R"({
        "schema": "engine.payoff/v1",
        "id": "COMBINATORS",
        "contract": {
            "type": "both",
            "children": [
                {
                    "type": "scale",
                    "factor": {"type": "constant", "value": 2.0},
                    "child": {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD", "amount": {"type": "constant", "value": 10.0}}}
                },
                {
                    "type": "give",
                    "child": {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD", "amount": {"type": "constant", "value": 5.0}}}
                },
                {
                    "type": "when",
                    "time": 1.0,
                    "child": {
                        "type": "if",
                        "condition": {"type": "eq", "left": {"type": "constant", "value": 1.0}, "right": {"type": "constant", "value": 1.0}, "tolerance": 1e-9},
                        "if_true": {"type": "cashflow", "currency": "USD", "amount": {"type": "constant", "value": 1.0}},
                        "if_false": {"type": "zero"}
                    }
                }
            ]
        }
    })";
    ParsedPayoffDocument doc = parse_payoff_document(json);
    MarketPath path;
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    CashflowLedger ledger = evaluator.evaluate(doc.contract, context);
    double total = 0.0;
    for (const auto& e : ledger) total += e.amount;
    EXPECT_DOUBLE_EQ(total, 20.0 - 5.0 + 1.0);
}

TEST(JsonParserTest, RejectsMissingSchema) {
    const char* json = R"({"id": "X", "contract": {"type": "zero"}})";
    EXPECT_THROW(parse_payoff_document(json), ParseError);
}

TEST(JsonParserTest, RejectsUnknownSchemaVersion) {
    const char* json = R"({"schema": "engine.payoff/v2", "id": "X", "contract": {"type": "zero"}})";
    EXPECT_THROW(parse_payoff_document(json), ParseError);
}

TEST(JsonParserTest, RejectsUnknownTopLevelField) {
    const char* json = R"({"schema": "engine.payoff/v1", "id": "X", "contract": {"type": "zero"}, "extra": 1})";
    EXPECT_THROW(parse_payoff_document(json), ParseError);
}

TEST(JsonParserTest, RejectsUnknownFieldInsideNode) {
    const char* json = R"({"schema": "engine.payoff/v1", "id": "X", "contract": {"type": "zero", "unexpected": true}})";
    EXPECT_THROW(parse_payoff_document(json), ParseError);
}

TEST(JsonParserTest, RejectsUnknownNodeType) {
    const char* json = R"({"schema": "engine.payoff/v1", "id": "X", "contract": {"type": "not_a_real_type"}})";
    EXPECT_THROW(parse_payoff_document(json), ParseError);
}

TEST(JsonParserTest, RejectsMalformedJson) {
    const char* json = R"({"schema": "engine.payoff/v1", "id": "X", "contract": {"type": "zero")"; // JSON truncado
    EXPECT_THROW(parse_payoff_document(json), ParseError);
}

TEST(JsonParserTest, RejectsDocumentExceedingMaxBytes) {
    const char* json = R"({"schema": "engine.payoff/v1", "id": "X", "contract": {"type": "zero"}})";
    ParseLimits limits;
    limits.max_bytes = 5;
    EXPECT_THROW(parse_payoff_document(json, limits), ParseError);
}

TEST(JsonParserTest, RejectsTreeExceedingMaxNodes) {
    const char* json = R"({"schema": "engine.payoff/v1", "id": "X", "contract":
        {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD", "amount": {"type": "constant", "value": 1.0}}}
    })";
    ParseLimits limits;
    limits.max_nodes = 2; // when+cashflow ya son 2; constant seria el tercero
    EXPECT_THROW(parse_payoff_document(json, limits), ParseError);
}

TEST(JsonParserTest, RejectsTreeExceedingMaxDepth) {
    const char* json = R"({"schema": "engine.payoff/v1", "id": "X", "contract":
        {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD", "amount":
            {"type": "neg", "operand": {"type": "neg", "operand": {"type": "constant", "value": 1.0}}}
        }}
    })";
    ParseLimits limits;
    limits.max_depth = 3;
    EXPECT_THROW(parse_payoff_document(json, limits), ParseError);
}

} // namespace
