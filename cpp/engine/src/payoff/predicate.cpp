#include "engine/payoff/predicate.hpp"

#include <utility>

namespace engine {
namespace payoff {

Greater::Greater(ScalarExprPtr left, ScalarExprPtr right)
    : Predicate(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Greater::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

Less::Less(ScalarExprPtr left, ScalarExprPtr right)
    : Predicate(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Less::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

GreaterEqual::GreaterEqual(ScalarExprPtr left, ScalarExprPtr right)
    : Predicate(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void GreaterEqual::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

LessEqual::LessEqual(ScalarExprPtr left, ScalarExprPtr right)
    : Predicate(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void LessEqual::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

Eq::Eq(ScalarExprPtr left, ScalarExprPtr right, double tolerance)
    : Predicate(allocate_node_id()), left_(std::move(left)), right_(std::move(right)), tolerance_(tolerance) {}
void Eq::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

All::All(std::vector<PredicatePtr> operands) : Predicate(allocate_node_id()), operands_(std::move(operands)) {}
void All::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

Any::Any(std::vector<PredicatePtr> operands) : Predicate(allocate_node_id()), operands_(std::move(operands)) {}
void Any::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

Not::Not(PredicatePtr operand) : Predicate(allocate_node_id()), operand_(std::move(operand)) {}
void Not::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

Between::Between(
    ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high, bool low_inclusive, bool high_inclusive
)
    : Predicate(allocate_node_id()),
      value_(std::move(value)),
      low_(std::move(low)),
      high_(std::move(high)),
      low_inclusive_(low_inclusive),
      high_inclusive_(high_inclusive) {}
void Between::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

EventOccurred::EventOccurred(EventId event) : Predicate(allocate_node_id()), event_(std::move(event)) {}
void EventOccurred::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

Before::Before(TimePoint time) : Predicate(allocate_node_id()), time_(time) {}
void Before::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

After::After(TimePoint time) : Predicate(allocate_node_id()), time_(time) {}
void After::accept(PredicateVisitor& visitor) const { visitor.visit(*this); }

PredicatePtr greater(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Greater>(std::move(left), std::move(right));
}
PredicatePtr less(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Less>(std::move(left), std::move(right));
}
PredicatePtr greater_equal(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const GreaterEqual>(std::move(left), std::move(right));
}
PredicatePtr less_equal(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const LessEqual>(std::move(left), std::move(right));
}
PredicatePtr eq(ScalarExprPtr left, ScalarExprPtr right, double tolerance) {
    return std::make_shared<const Eq>(std::move(left), std::move(right), tolerance);
}
PredicatePtr all_of(std::vector<PredicatePtr> operands) {
    return std::make_shared<const All>(std::move(operands));
}
PredicatePtr any_of(std::vector<PredicatePtr> operands) {
    return std::make_shared<const Any>(std::move(operands));
}
PredicatePtr negate(PredicatePtr operand) { return std::make_shared<const Not>(std::move(operand)); }
PredicatePtr between(
    ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high, bool low_inclusive, bool high_inclusive
) {
    return std::make_shared<const Between>(
        std::move(value), std::move(low), std::move(high), low_inclusive, high_inclusive
    );
}
PredicatePtr event_occurred(EventId event) { return std::make_shared<const EventOccurred>(std::move(event)); }
PredicatePtr before(TimePoint time) { return std::make_shared<const Before>(time); }
PredicatePtr after(TimePoint time) { return std::make_shared<const After>(time); }

} // namespace payoff
} // namespace engine
