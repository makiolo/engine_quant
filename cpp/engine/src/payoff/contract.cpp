#include "engine/payoff/contract.hpp"

#include <stdexcept>
#include <utility>

namespace engine {
namespace payoff {

Zero::Zero() : Contract(allocate_node_id()) {}
void Zero::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

Cashflow::Cashflow(Currency currency, ScalarExprPtr amount)
    : Contract(allocate_node_id()), currency_(std::move(currency)), amount_(std::move(amount)) {}
void Cashflow::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

Give::Give(ContractPtr child) : Contract(allocate_node_id()), child_(std::move(child)) {}
void Give::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

Both::Both(std::vector<ContractPtr> children) : Contract(allocate_node_id()), children_(std::move(children)) {
    if (children_.empty()) {
        throw std::invalid_argument("Both requiere al menos un hijo (use Zero para 'ningun cashflow')");
    }
}
void Both::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

Scale::Scale(ScalarExprPtr factor, ContractPtr child)
    : Contract(allocate_node_id()), factor_(std::move(factor)), child_(std::move(child)) {}
void Scale::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

If::If(PredicatePtr condition, ContractPtr if_true, ContractPtr if_false)
    : Contract(allocate_node_id()),
      condition_(std::move(condition)),
      if_true_(std::move(if_true)),
      if_false_(std::move(if_false)) {}
void If::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

When::When(TimePoint time, ContractPtr child) : Contract(allocate_node_id()), time_(time), child_(std::move(child)) {}
void When::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

Trigger::Trigger(TriggerSpec spec, ContractPtr on_hit, ContractPtr on_miss)
    : Contract(allocate_node_id()), spec_(std::move(spec)), on_hit_(std::move(on_hit)), on_miss_(std::move(on_miss)) {}
void Trigger::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

Exercise::Exercise(EventId id, std::vector<TimePoint> dates, ScalarExprPtr exercise_value, ContractPtr continuation)
    : Contract(allocate_node_id()),
      id_(std::move(id)),
      dates_(std::move(dates)),
      exercise_value_(std::move(exercise_value)),
      continuation_(std::move(continuation)) {}
void Exercise::accept(ContractVisitor& visitor) const { visitor.visit(*this); }

ContractPtr zero() { return std::make_shared<const Zero>(); }
ContractPtr cashflow(Currency currency, ScalarExprPtr amount) {
    return std::make_shared<const Cashflow>(std::move(currency), std::move(amount));
}
ContractPtr give(ContractPtr child) { return std::make_shared<const Give>(std::move(child)); }
ContractPtr both(std::vector<ContractPtr> children) { return std::make_shared<const Both>(std::move(children)); }
ContractPtr scale(ScalarExprPtr factor, ContractPtr child) {
    return std::make_shared<const Scale>(std::move(factor), std::move(child));
}
ContractPtr if_(PredicatePtr condition, ContractPtr if_true, ContractPtr if_false) {
    return std::make_shared<const If>(std::move(condition), std::move(if_true), std::move(if_false));
}
ContractPtr when(TimePoint time, ContractPtr child) {
    return std::make_shared<const When>(time, std::move(child));
}
ContractPtr trigger(TriggerSpec spec, ContractPtr on_hit, ContractPtr on_miss) {
    return std::make_shared<const Trigger>(std::move(spec), std::move(on_hit), std::move(on_miss));
}
ContractPtr exercise(
    EventId id, std::vector<TimePoint> dates, ScalarExprPtr exercise_value, ContractPtr continuation
) {
    return std::make_shared<const Exercise>(
        std::move(id), std::move(dates), std::move(exercise_value), std::move(continuation)
    );
}

} // namespace payoff
} // namespace engine
