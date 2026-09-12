// Tests estructurales de los nodos `Contract` sin estado (PLAN_PRODUCTS.md §3.4, §12 Fase 1):
// forma/campos de cada nodo vía sus builders y doble dispatch de `accept()` (ADR-P0-06).
//
// Las propiedades semánticas sobre el ledger agregado (Both(x,Zero)==x, Give(Give(x))==x,
// Scale(0,x)==Zero, invariancia a la permutación de hijos de Both, Scale(-1,x)==Give(x)) se
// testean en test_scenario_evaluator_vanilla.cpp: dependen de `ScenarioEvaluator`, que no
// existe todavía en este commit (Fase 1 lo introduce después, ver PLAN_PRODUCTS.md §12).

#include <gtest/gtest.h>
#include <stdexcept>

#include "engine/payoff/contract.hpp"

namespace {

using namespace engine::payoff;

template <typename T>
const T& as(const ContractPtr& node) {
    const T* casted = dynamic_cast<const T*>(node.get());
    EXPECT_NE(casted, nullptr);
    return *casted;
}

TEST(ContractBasic, ZeroHasNoFields) {
    ContractPtr node = zero();
    EXPECT_NE(dynamic_cast<const Zero*>(node.get()), nullptr);
}

TEST(ContractBasic, CashflowHoldsCurrencyAndAmount) {
    auto amount = constant(100.0);
    auto node = as<Cashflow>(cashflow(Currency{"USD"}, amount));
    EXPECT_EQ(node.currency(), Currency{"USD"});
    EXPECT_EQ(node.amount(), amount);
}

TEST(ContractBasic, GiveHoldsChild) {
    auto child = zero();
    EXPECT_EQ(as<Give>(give(child)).child(), child);
}

TEST(ContractBasic, BothHoldsChildrenInOrder) {
    auto c1 = zero();
    auto c2 = cashflow(Currency{"USD"}, constant(1.0));
    auto node = as<Both>(both({c1, c2}));
    ASSERT_EQ(node.children().size(), 2u);
    EXPECT_EQ(node.children()[0], c1);
    EXPECT_EQ(node.children()[1], c2);
}

TEST(ContractBasic, BothRejectsEmptyChildren) {
    EXPECT_THROW(both({}), std::invalid_argument);
}

TEST(ContractBasic, ScaleHoldsFactorAndChild) {
    auto factor = constant(2.0);
    auto child = zero();
    auto node = as<Scale>(scale(factor, child));
    EXPECT_EQ(node.factor(), factor);
    EXPECT_EQ(node.child(), child);
}

TEST(ContractBasic, IfHoldsConditionAndBranches) {
    auto condition = greater(constant(1.0), constant(0.0));
    auto if_true = cashflow(Currency{"USD"}, constant(1.0));
    auto if_false = zero();
    auto node = as<If>(if_(condition, if_true, if_false));
    EXPECT_EQ(node.condition(), condition);
    EXPECT_EQ(node.if_true(), if_true);
    EXPECT_EQ(node.if_false(), if_false);
}

TEST(ContractBasic, WhenHoldsTimeAndChild) {
    auto child = cashflow(Currency{"USD"}, constant(1.0));
    auto node = as<When>(when(TimePoint{1.0}, child));
    EXPECT_DOUBLE_EQ(node.time().year_fraction, 1.0);
    EXPECT_EQ(node.child(), child);
}

TEST(ContractBasic, TriggerHoldsSpecAndBranches) {
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{0.5}, TimePoint{1.0}}, greater_equal(constant(1.0), constant(0.0)),
        Monitoring::Discrete, Settlement::AtScheduledPayment, 0, true
    };
    auto on_hit = cashflow(Currency{"USD"}, constant(1.0));
    auto on_miss = zero();
    auto node = as<Trigger>(trigger(spec, on_hit, on_miss));
    EXPECT_EQ(node.spec().id, EventId{"UI"});
    ASSERT_EQ(node.spec().monitoring_times.size(), 2u);
    EXPECT_EQ(node.spec().monitoring, Monitoring::Discrete);
    EXPECT_EQ(node.spec().settlement, Settlement::AtScheduledPayment);
    EXPECT_EQ(node.spec().priority, 0);
    EXPECT_TRUE(node.spec().latch);
    EXPECT_EQ(node.on_hit(), on_hit);
    EXPECT_EQ(node.on_miss(), on_miss);
}

TEST(ContractBasic, ExerciseHoldsMinimalFields) {
    auto exercise_value = constant(0.0);
    auto continuation = zero();
    auto node = as<Exercise>(exercise(EventId{"EX"}, {TimePoint{1.0}, TimePoint{2.0}}, exercise_value, continuation));
    EXPECT_EQ(node.id(), EventId{"EX"});
    ASSERT_EQ(node.dates().size(), 2u);
    EXPECT_EQ(node.exercise_value(), exercise_value);
    EXPECT_EQ(node.continuation(), continuation);
}

class TagVisitor : public ContractVisitor {
public:
    std::string tag;
    void visit(const Zero&) override { tag = "zero"; }
    void visit(const Cashflow&) override { tag = "cashflow"; }
    void visit(const Give&) override { tag = "give"; }
    void visit(const Both&) override { tag = "both"; }
    void visit(const Scale&) override { tag = "scale"; }
    void visit(const If&) override { tag = "if"; }
    void visit(const When&) override { tag = "when"; }
    void visit(const Trigger&) override { tag = "trigger"; }
    void visit(const Exercise&) override { tag = "exercise"; }
};

TEST(ContractBasic, AcceptDispatchesToConcreteVisitOverload) {
    TagVisitor visitor;
    ContractPtr node = give(zero());
    node->accept(visitor);
    EXPECT_EQ(visitor.tag, "give");
}

} // namespace
