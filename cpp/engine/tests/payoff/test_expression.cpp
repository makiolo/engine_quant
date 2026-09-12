// Tests de `ScalarExpr` v1 (PLAN_PRODUCTS.md §3.2, §12 Fase 1): un TEST por nodo confirmando
// que el builder produce el tipo concreto correcto con los campos esperados, más un test de
// mecanismo que confirma el doble dispatch de `accept(ScalarVisitor&)` (ADR-P0-06).

#include <gtest/gtest.h>

#include "engine/payoff/expression.hpp"

namespace {

using namespace engine::payoff;

template <typename T>
const T& as(const ScalarExprPtr& expr) {
    const T* node = dynamic_cast<const T*>(expr.get());
    EXPECT_NE(node, nullptr);
    return *node;
}

TEST(ScalarExpr, ConstantHoldsValue) {
    EXPECT_DOUBLE_EQ(as<Constant>(constant(42.0)).value(), 42.0);
}

TEST(ScalarExpr, ParameterHoldsName) {
    EXPECT_EQ(as<Parameter>(parameter("strike")).name(), "strike");
}

TEST(ScalarExpr, FixingHoldsObservableAndTime) {
    auto node = as<Fixing>(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}));
    EXPECT_EQ(node.observable(), ObservableId{"EQ.SPOT.AAPL"});
    EXPECT_DOUBLE_EQ(node.time().year_fraction, 1.0);
}

TEST(ScalarExpr, CurrentHoldsObservable) {
    EXPECT_EQ(as<Current>(current(ObservableId{"EQ.SPOT.AAPL"})).observable(), ObservableId{"EQ.SPOT.AAPL"});
}

TEST(ScalarExpr, AddHoldsLeftAndRight) {
    auto l = constant(1.0);
    auto r = constant(2.0);
    auto node = as<Add>(add(l, r));
    EXPECT_EQ(node.left(), l);
    EXPECT_EQ(node.right(), r);
}

TEST(ScalarExpr, SubHoldsLeftAndRight) {
    auto node = as<Sub>(sub(constant(5.0), constant(3.0)));
    EXPECT_DOUBLE_EQ(as<Constant>(node.left()).value(), 5.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.right()).value(), 3.0);
}

TEST(ScalarExpr, MulHoldsLeftAndRight) {
    auto node = as<Mul>(mul(constant(2.0), constant(3.0)));
    EXPECT_DOUBLE_EQ(as<Constant>(node.left()).value(), 2.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.right()).value(), 3.0);
}

TEST(ScalarExpr, DivHoldsLeftAndRight) {
    auto node = as<Div>(div(constant(6.0), constant(2.0)));
    EXPECT_DOUBLE_EQ(as<Constant>(node.left()).value(), 6.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.right()).value(), 2.0);
}

TEST(ScalarExpr, NegHoldsOperand) {
    EXPECT_DOUBLE_EQ(as<Constant>(as<Neg>(neg(constant(4.0))).operand()).value(), 4.0);
}

TEST(ScalarExpr, AbsHoldsOperand) {
    EXPECT_DOUBLE_EQ(as<Constant>(as<Abs>(abs(constant(-4.0))).operand()).value(), -4.0);
}

TEST(ScalarExpr, ExpHoldsOperand) {
    EXPECT_DOUBLE_EQ(as<Constant>(as<Exp>(exp(constant(1.0))).operand()).value(), 1.0);
}

TEST(ScalarExpr, LogHoldsOperand) {
    EXPECT_DOUBLE_EQ(as<Constant>(as<Log>(log(constant(1.0))).operand()).value(), 1.0);
}

TEST(ScalarExpr, PowHoldsBaseAndExponent) {
    auto node = as<Pow>(pow(constant(2.0), constant(3.0)));
    EXPECT_DOUBLE_EQ(as<Constant>(node.base()).value(), 2.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.exponent()).value(), 3.0);
}

TEST(ScalarExpr, MinimumBuildsMinNode) {
    auto node = as<Min>(minimum(constant(1.0), constant(2.0)));
    EXPECT_DOUBLE_EQ(as<Constant>(node.left()).value(), 1.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.right()).value(), 2.0);
}

TEST(ScalarExpr, MaximumBuildsMaxNode) {
    auto node = as<Max>(maximum(constant(1.0), constant(2.0)));
    EXPECT_DOUBLE_EQ(as<Constant>(node.left()).value(), 1.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.right()).value(), 2.0);
}

TEST(ScalarExpr, ClampHoldsValueLowHigh) {
    auto node = as<Clamp>(clamp(constant(5.0), constant(0.0), constant(1.0)));
    EXPECT_DOUBLE_EQ(as<Constant>(node.value()).value(), 5.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.low()).value(), 0.0);
    EXPECT_DOUBLE_EQ(as<Constant>(node.high()).value(), 1.0);
}

TEST(ScalarExpr, AverageHoldsScheduleAndWeights) {
    auto node = as<Average>(average(
        ObservableId{"EQ.SPOT.AAPL"}, {TimePoint{0.25}, TimePoint{0.5}}, {0.5, 0.5}
    ));
    ASSERT_EQ(node.schedule().size(), 2u);
    EXPECT_DOUBLE_EQ(node.schedule()[0].year_fraction, 0.25);
    ASSERT_EQ(node.weights().size(), 2u);
    EXPECT_DOUBLE_EQ(node.weights()[1], 0.5);
}

TEST(ScalarExpr, RunningMinHoldsObservable) {
    EXPECT_EQ(as<RunningMin>(running_min(ObservableId{"EQ.SPOT.AAPL"})).observable(), ObservableId{"EQ.SPOT.AAPL"});
}

TEST(ScalarExpr, RunningMaxHoldsObservable) {
    EXPECT_EQ(as<RunningMax>(running_max(ObservableId{"EQ.SPOT.AAPL"})).observable(), ObservableId{"EQ.SPOT.AAPL"});
}

TEST(ScalarExpr, EventTimeHoldsEventId) {
    EXPECT_EQ(as<EventTime>(event_time(EventId{"UI"})).event(), EventId{"UI"});
}

TEST(ScalarExpr, EventValueHoldsEventAndObservable) {
    auto node = as<EventValue>(event_value(EventId{"UI"}, ObservableId{"EQ.SPOT.AAPL"}));
    EXPECT_EQ(node.event(), EventId{"UI"});
    EXPECT_EQ(node.observable(), ObservableId{"EQ.SPOT.AAPL"});
}

TEST(ScalarExpr, DiscountFactorHoldsCurveAndTimes) {
    auto node = as<DiscountFactor>(discount_factor(CurveId{"IR.DF.EUR.OIS"}, TimePoint{0.0}, TimePoint{1.0}));
    EXPECT_EQ(node.curve(), CurveId{"IR.DF.EUR.OIS"});
    EXPECT_DOUBLE_EQ(node.from().year_fraction, 0.0);
    EXPECT_DOUBLE_EQ(node.to().year_fraction, 1.0);
}

TEST(ScalarExpr, FxConversionHoldsCurrenciesAndTime) {
    auto node = as<FxConversion>(fx_conversion(Currency{"EUR"}, Currency{"USD"}, TimePoint{1.0}));
    EXPECT_EQ(node.from_currency(), Currency{"EUR"});
    EXPECT_EQ(node.to_currency(), Currency{"USD"});
    EXPECT_DOUBLE_EQ(node.time().year_fraction, 1.0);
}

TEST(ScalarExpr, OperatorSugarMatchesBuilders) {
    auto a = constant(3.0);
    auto b = constant(4.0);
    EXPECT_DOUBLE_EQ(as<Constant>(as<Add>(a + b).left()).value(), 3.0);
    EXPECT_DOUBLE_EQ(as<Constant>(as<Sub>(a - b).right()).value(), 4.0);
    EXPECT_DOUBLE_EQ(as<Constant>(as<Mul>(a * b).right()).value(), 4.0);
    EXPECT_DOUBLE_EQ(as<Constant>(as<Div>(a / b).left()).value(), 3.0);
    EXPECT_DOUBLE_EQ(as<Constant>(as<Neg>(-a).operand()).value(), 3.0);
}

// Confirma el mecanismo de doble dispatch (ADR-P0-06): un `ScalarVisitor` concreto recibe la
// llamada `visit` correspondiente al tipo real del nodo, no al tipo estático `ScalarExpr`.
class TagVisitor : public ScalarVisitor {
public:
    std::string tag;
    void visit(const Constant&) override { tag = "constant"; }
    void visit(const Parameter&) override { tag = "parameter"; }
    void visit(const Fixing&) override { tag = "fixing"; }
    void visit(const Current&) override { tag = "current"; }
    void visit(const Add&) override { tag = "add"; }
    void visit(const Sub&) override { tag = "sub"; }
    void visit(const Mul&) override { tag = "mul"; }
    void visit(const Div&) override { tag = "div"; }
    void visit(const Neg&) override { tag = "neg"; }
    void visit(const Abs&) override { tag = "abs"; }
    void visit(const Exp&) override { tag = "exp"; }
    void visit(const Log&) override { tag = "log"; }
    void visit(const Pow&) override { tag = "pow"; }
    void visit(const Min&) override { tag = "min"; }
    void visit(const Max&) override { tag = "max"; }
    void visit(const Clamp&) override { tag = "clamp"; }
    void visit(const Average&) override { tag = "average"; }
    void visit(const RunningMin&) override { tag = "running_min"; }
    void visit(const RunningMax&) override { tag = "running_max"; }
    void visit(const EventTime&) override { tag = "event_time"; }
    void visit(const EventValue&) override { tag = "event_value"; }
    void visit(const DiscountFactor&) override { tag = "discount_factor"; }
    void visit(const FxConversion&) override { tag = "fx_conversion"; }
};

TEST(ScalarExpr, AcceptDispatchesToConcreteVisitOverload) {
    TagVisitor visitor;
    ScalarExprPtr node = add(constant(1.0), constant(2.0));
    const ScalarExpr& base_ref = *node;
    base_ref.accept(visitor);
    EXPECT_EQ(visitor.tag, "add");

    node = maximum(constant(1.0), constant(0.0));
    node->accept(visitor);
    EXPECT_EQ(visitor.tag, "max");
}

} // namespace
