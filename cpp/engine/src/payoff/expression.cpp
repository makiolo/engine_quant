#include "engine/payoff/expression.hpp"

#include <utility>

namespace engine {
namespace payoff {

Constant::Constant(double value) : ScalarExpr(allocate_node_id()), value_(value) {}
void Constant::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Parameter::Parameter(std::string name) : ScalarExpr(allocate_node_id()), name_(std::move(name)) {}
void Parameter::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Fixing::Fixing(ObservableId observable, TimePoint time)
    : ScalarExpr(allocate_node_id()), observable_(std::move(observable)), time_(time) {}
void Fixing::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Current::Current(ObservableId observable) : ScalarExpr(allocate_node_id()), observable_(std::move(observable)) {}
void Current::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Add::Add(ScalarExprPtr left, ScalarExprPtr right)
    : ScalarExpr(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Add::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Sub::Sub(ScalarExprPtr left, ScalarExprPtr right)
    : ScalarExpr(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Sub::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Mul::Mul(ScalarExprPtr left, ScalarExprPtr right)
    : ScalarExpr(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Mul::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Div::Div(ScalarExprPtr left, ScalarExprPtr right)
    : ScalarExpr(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Div::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Neg::Neg(ScalarExprPtr operand) : ScalarExpr(allocate_node_id()), operand_(std::move(operand)) {}
void Neg::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Abs::Abs(ScalarExprPtr operand) : ScalarExpr(allocate_node_id()), operand_(std::move(operand)) {}
void Abs::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Exp::Exp(ScalarExprPtr operand) : ScalarExpr(allocate_node_id()), operand_(std::move(operand)) {}
void Exp::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Log::Log(ScalarExprPtr operand) : ScalarExpr(allocate_node_id()), operand_(std::move(operand)) {}
void Log::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Pow::Pow(ScalarExprPtr base, ScalarExprPtr exponent)
    : ScalarExpr(allocate_node_id()), base_(std::move(base)), exponent_(std::move(exponent)) {}
void Pow::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Min::Min(ScalarExprPtr left, ScalarExprPtr right)
    : ScalarExpr(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Min::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Max::Max(ScalarExprPtr left, ScalarExprPtr right)
    : ScalarExpr(allocate_node_id()), left_(std::move(left)), right_(std::move(right)) {}
void Max::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Clamp::Clamp(ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high)
    : ScalarExpr(allocate_node_id()), value_(std::move(value)), low_(std::move(low)), high_(std::move(high)) {}
void Clamp::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

Average::Average(ObservableId observable, std::vector<TimePoint> schedule, std::vector<double> weights)
    : ScalarExpr(allocate_node_id()),
      observable_(std::move(observable)),
      schedule_(std::move(schedule)),
      weights_(std::move(weights)) {}
void Average::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

RunningMin::RunningMin(ObservableId observable) : ScalarExpr(allocate_node_id()), observable_(std::move(observable)) {}
void RunningMin::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

RunningMax::RunningMax(ObservableId observable) : ScalarExpr(allocate_node_id()), observable_(std::move(observable)) {}
void RunningMax::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

EventTime::EventTime(EventId event) : ScalarExpr(allocate_node_id()), event_(std::move(event)) {}
void EventTime::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

EventValue::EventValue(EventId event, ObservableId observable)
    : ScalarExpr(allocate_node_id()), event_(std::move(event)), observable_(std::move(observable)) {}
void EventValue::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

DiscountFactor::DiscountFactor(CurveId curve, TimePoint from, TimePoint to)
    : ScalarExpr(allocate_node_id()), curve_(std::move(curve)), from_(from), to_(to) {}
void DiscountFactor::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

FxConversion::FxConversion(Currency from_currency, Currency to_currency, TimePoint time)
    : ScalarExpr(allocate_node_id()),
      from_currency_(std::move(from_currency)),
      to_currency_(std::move(to_currency)),
      time_(time) {}
void FxConversion::accept(ScalarVisitor& visitor) const { visitor.visit(*this); }

ScalarExprPtr constant(double value) { return std::make_shared<const Constant>(value); }
ScalarExprPtr parameter(std::string name) { return std::make_shared<const Parameter>(std::move(name)); }
ScalarExprPtr fixing(ObservableId observable, TimePoint time) {
    return std::make_shared<const Fixing>(std::move(observable), time);
}
ScalarExprPtr current(ObservableId observable) { return std::make_shared<const Current>(std::move(observable)); }
ScalarExprPtr add(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Add>(std::move(left), std::move(right));
}
ScalarExprPtr sub(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Sub>(std::move(left), std::move(right));
}
ScalarExprPtr mul(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Mul>(std::move(left), std::move(right));
}
ScalarExprPtr div(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Div>(std::move(left), std::move(right));
}
ScalarExprPtr neg(ScalarExprPtr operand) { return std::make_shared<const Neg>(std::move(operand)); }
ScalarExprPtr abs(ScalarExprPtr operand) { return std::make_shared<const Abs>(std::move(operand)); }
ScalarExprPtr exp(ScalarExprPtr operand) { return std::make_shared<const Exp>(std::move(operand)); }
ScalarExprPtr log(ScalarExprPtr operand) { return std::make_shared<const Log>(std::move(operand)); }
ScalarExprPtr pow(ScalarExprPtr base, ScalarExprPtr exponent) {
    return std::make_shared<const Pow>(std::move(base), std::move(exponent));
}
ScalarExprPtr minimum(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Min>(std::move(left), std::move(right));
}
ScalarExprPtr maximum(ScalarExprPtr left, ScalarExprPtr right) {
    return std::make_shared<const Max>(std::move(left), std::move(right));
}
ScalarExprPtr clamp(ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high) {
    return std::make_shared<const Clamp>(std::move(value), std::move(low), std::move(high));
}
ScalarExprPtr average(ObservableId observable, std::vector<TimePoint> schedule, std::vector<double> weights) {
    return std::make_shared<const Average>(std::move(observable), std::move(schedule), std::move(weights));
}
ScalarExprPtr running_min(ObservableId observable) { return std::make_shared<const RunningMin>(std::move(observable)); }
ScalarExprPtr running_max(ObservableId observable) { return std::make_shared<const RunningMax>(std::move(observable)); }
ScalarExprPtr event_time(EventId event) { return std::make_shared<const EventTime>(std::move(event)); }
ScalarExprPtr event_value(EventId event, ObservableId observable) {
    return std::make_shared<const EventValue>(std::move(event), std::move(observable));
}
ScalarExprPtr discount_factor(CurveId curve, TimePoint from, TimePoint to) {
    return std::make_shared<const DiscountFactor>(std::move(curve), from, to);
}
ScalarExprPtr fx_conversion(Currency from_currency, Currency to_currency, TimePoint time) {
    return std::make_shared<const FxConversion>(std::move(from_currency), std::move(to_currency), time);
}

ScalarExprPtr operator+(ScalarExprPtr left, ScalarExprPtr right) { return add(std::move(left), std::move(right)); }
ScalarExprPtr operator-(ScalarExprPtr left, ScalarExprPtr right) { return sub(std::move(left), std::move(right)); }
ScalarExprPtr operator*(ScalarExprPtr left, ScalarExprPtr right) { return mul(std::move(left), std::move(right)); }
ScalarExprPtr operator/(ScalarExprPtr left, ScalarExprPtr right) { return div(std::move(left), std::move(right)); }
ScalarExprPtr operator-(ScalarExprPtr operand) { return neg(std::move(operand)); }

} // namespace payoff
} // namespace engine
