#include "engine/payoff/dependency_visitor.hpp"

namespace engine {
namespace payoff {

DependencyReport DependencyVisitor::analyze(const ContractPtr& root) {
    report_ = DependencyReport{};
    if (root) root->accept(*this);
    return report_;
}

void DependencyVisitor::visit_scalar(const ScalarExprPtr& expr) {
    if (expr) expr->accept(*this);
}

void DependencyVisitor::visit_predicate(const PredicatePtr& pred) {
    if (pred) pred->accept(*this);
}

void DependencyVisitor::visit_contract(const ContractPtr& node) {
    if (node) node->accept(*this);
}

// ---- ScalarVisitor ----

void DependencyVisitor::visit(const Constant&) {}
void DependencyVisitor::visit(const Parameter&) {}

void DependencyVisitor::visit(const Fixing& node) {
    report_.observables.insert(node.observable());
    report_.fixing_dates.insert(node.time());
}

void DependencyVisitor::visit(const Current& node) { report_.observables.insert(node.observable()); }

void DependencyVisitor::visit(const Add& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Sub& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Mul& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Div& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Neg& node) { visit_scalar(node.operand()); }
void DependencyVisitor::visit(const Abs& node) { visit_scalar(node.operand()); }
void DependencyVisitor::visit(const Exp& node) { visit_scalar(node.operand()); }
void DependencyVisitor::visit(const Log& node) { visit_scalar(node.operand()); }
void DependencyVisitor::visit(const Pow& node) {
    visit_scalar(node.base());
    visit_scalar(node.exponent());
}
void DependencyVisitor::visit(const Min& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Max& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Clamp& node) {
    visit_scalar(node.value());
    visit_scalar(node.low());
    visit_scalar(node.high());
}

void DependencyVisitor::visit(const Average& node) {
    report_.observables.insert(node.observable());
    for (TimePoint t : node.schedule()) report_.fixing_dates.insert(t);
}

void DependencyVisitor::visit(const RunningMin& node) { report_.observables.insert(node.observable()); }
void DependencyVisitor::visit(const RunningMax& node) { report_.observables.insert(node.observable()); }

void DependencyVisitor::visit(const EventTime& node) { report_.events.insert(node.event()); }

void DependencyVisitor::visit(const EventValue& node) {
    report_.events.insert(node.event());
    report_.observables.insert(node.observable());
}

void DependencyVisitor::visit(const DiscountFactor& node) {
    report_.curves.insert(node.curve());
    report_.fixing_dates.insert(node.from());
    report_.fixing_dates.insert(node.to());
}

void DependencyVisitor::visit(const FxConversion& node) {
    report_.currencies.insert(node.from_currency());
    report_.currencies.insert(node.to_currency());
    report_.fixing_dates.insert(node.time());
}

// ---- PredicateVisitor ----

void DependencyVisitor::visit(const Greater& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Less& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const GreaterEqual& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const LessEqual& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const Eq& node) {
    visit_scalar(node.left());
    visit_scalar(node.right());
}
void DependencyVisitor::visit(const All& node) {
    for (const auto& operand : node.operands()) visit_predicate(operand);
}
void DependencyVisitor::visit(const Any& node) {
    for (const auto& operand : node.operands()) visit_predicate(operand);
}
void DependencyVisitor::visit(const Not& node) { visit_predicate(node.operand()); }
void DependencyVisitor::visit(const Between& node) {
    visit_scalar(node.value());
    visit_scalar(node.low());
    visit_scalar(node.high());
}
void DependencyVisitor::visit(const EventOccurred& node) { report_.events.insert(node.event()); }
void DependencyVisitor::visit(const Before&) {}
void DependencyVisitor::visit(const After&) {}

// ---- ContractVisitor ----

void DependencyVisitor::visit(const Zero&) {}

void DependencyVisitor::visit(const Cashflow& node) {
    report_.currencies.insert(node.currency());
    visit_scalar(node.amount());
}

void DependencyVisitor::visit(const Give& node) { visit_contract(node.child()); }

void DependencyVisitor::visit(const Both& node) {
    for (const auto& child : node.children()) visit_contract(child);
}

void DependencyVisitor::visit(const Scale& node) {
    visit_scalar(node.factor());
    visit_contract(node.child());
}

void DependencyVisitor::visit(const If& node) {
    visit_predicate(node.condition());
    visit_contract(node.if_true());
    visit_contract(node.if_false());
}

void DependencyVisitor::visit(const When& node) { visit_contract(node.child()); }

void DependencyVisitor::visit(const Trigger& node) {
    report_.events.insert(node.spec().id);
    for (TimePoint t : node.spec().monitoring_times) report_.fixing_dates.insert(t);
    visit_predicate(node.spec().condition);
    visit_contract(node.on_hit());
    visit_contract(node.on_miss());
}

void DependencyVisitor::visit(const Exercise& node) {
    report_.events.insert(node.id());
    for (TimePoint t : node.dates()) report_.fixing_dates.insert(t);
    visit_scalar(node.exercise_value());
    visit_contract(node.continuation());
}

} // namespace payoff
} // namespace engine
