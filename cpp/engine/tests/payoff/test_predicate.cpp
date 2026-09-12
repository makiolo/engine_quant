// Tests de `Predicate` v1 (PLAN_PRODUCTS.md §3.3, §12 Fase 1): un TEST por nodo, más la
// tolerancia de `Eq` y el desempate lexicográfico de `EventId` usado por `EventOccurred`.

#include <gtest/gtest.h>

#include "engine/payoff/predicate.hpp"

namespace {

using namespace engine::payoff;

template <typename T>
const T& as(const PredicatePtr& pred) {
    const T* node = dynamic_cast<const T*>(pred.get());
    EXPECT_NE(node, nullptr);
    return *node;
}

TEST(PredicateTest, GreaterHoldsLeftAndRight) {
    auto l = constant(1.0);
    auto r = constant(2.0);
    auto node = as<Greater>(greater(l, r));
    EXPECT_EQ(node.left(), l);
    EXPECT_EQ(node.right(), r);
}

TEST(PredicateTest, LessHoldsLeftAndRight) {
    auto node = as<Less>(less(constant(1.0), constant(2.0)));
    EXPECT_DOUBLE_EQ(dynamic_cast<const Constant&>(*node.left()).value(), 1.0);
}

TEST(PredicateTest, GreaterEqualHoldsLeftAndRight) {
    auto node = as<GreaterEqual>(greater_equal(constant(1.0), constant(2.0)));
    EXPECT_DOUBLE_EQ(dynamic_cast<const Constant&>(*node.right()).value(), 2.0);
}

TEST(PredicateTest, LessEqualHoldsLeftAndRight) {
    auto node = as<LessEqual>(less_equal(constant(1.0), constant(2.0)));
    EXPECT_DOUBLE_EQ(dynamic_cast<const Constant&>(*node.right()).value(), 2.0);
}

TEST(PredicateTest, EqHoldsToleranceExplicitly) {
    auto node = as<Eq>(eq(constant(1.0), constant(1.0 + 1e-7), 1e-6));
    EXPECT_DOUBLE_EQ(node.tolerance(), 1e-6);
}

TEST(PredicateTest, AllHoldsOperandsInOrder) {
    auto p1 = greater(constant(2.0), constant(1.0));
    auto p2 = less(constant(1.0), constant(2.0));
    auto node = as<All>(all_of({p1, p2}));
    ASSERT_EQ(node.operands().size(), 2u);
    EXPECT_EQ(node.operands()[0], p1);
    EXPECT_EQ(node.operands()[1], p2);
}

TEST(PredicateTest, AnyHoldsOperandsInOrder) {
    auto p1 = greater(constant(2.0), constant(1.0));
    auto p2 = less(constant(2.0), constant(1.0));
    auto node = as<Any>(any_of({p1, p2}));
    ASSERT_EQ(node.operands().size(), 2u);
    EXPECT_EQ(node.operands()[0], p1);
}

TEST(PredicateTest, NegateHoldsOperand) {
    auto p = greater(constant(2.0), constant(1.0));
    EXPECT_EQ(as<Not>(negate(p)).operand(), p);
}

TEST(PredicateTest, BetweenHoldsBoundsAndInclusivity) {
    auto node = as<Between>(between(constant(0.5), constant(0.0), constant(1.0), true, false));
    EXPECT_TRUE(node.low_inclusive());
    EXPECT_FALSE(node.high_inclusive());
}

TEST(PredicateTest, EventOccurredHoldsEventId) {
    EXPECT_EQ(as<EventOccurred>(event_occurred(EventId{"UI"})).event(), EventId{"UI"});
}

TEST(PredicateTest, BeforeHoldsTime) {
    EXPECT_DOUBLE_EQ(as<Before>(before(TimePoint{0.5})).time().year_fraction, 0.5);
}

TEST(PredicateTest, AfterHoldsTime) {
    EXPECT_DOUBLE_EQ(as<After>(after(TimePoint{0.5})).time().year_fraction, 0.5);
}

// Confirma el doble dispatch (ADR-P0-06) igual que en test_expression.cpp, para la jerarquía
// de Predicate.
class TagVisitor : public PredicateVisitor {
public:
    std::string tag;
    void visit(const Greater&) override { tag = "greater"; }
    void visit(const Less&) override { tag = "less"; }
    void visit(const GreaterEqual&) override { tag = "greater_equal"; }
    void visit(const LessEqual&) override { tag = "less_equal"; }
    void visit(const Eq&) override { tag = "eq"; }
    void visit(const All&) override { tag = "all"; }
    void visit(const Any&) override { tag = "any"; }
    void visit(const Not&) override { tag = "not"; }
    void visit(const Between&) override { tag = "between"; }
    void visit(const EventOccurred&) override { tag = "event_occurred"; }
    void visit(const Before&) override { tag = "before"; }
    void visit(const After&) override { tag = "after"; }
};

TEST(PredicateTest, AcceptDispatchesToConcreteVisitOverload) {
    TagVisitor visitor;
    PredicatePtr node = negate(event_occurred(EventId{"UI"}));
    node->accept(visitor);
    EXPECT_EQ(visitor.tag, "not");
}

} // namespace
