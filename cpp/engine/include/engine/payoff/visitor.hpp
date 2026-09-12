#pragma once

namespace engine {
namespace payoff {

// Forward declarations de los nodos de `ScalarExpr` (PLAN_PRODUCTS.md §3.2).
class Constant;
class Parameter;
class Fixing;
class Current;
class Add;
class Sub;
class Mul;
class Div;
class Neg;
class Abs;
class Exp;
class Log;
class Pow;
class Min;
class Max;
class Clamp;
class Average;
class RunningMin;
class RunningMax;
class EventTime;
class EventValue;
class DiscountFactor;
class FxConversion;

// Doble dispatch (ADR-P0-06): cada nodo implementa `accept(ScalarVisitor&)` invocando el
// overload `visit` correspondiente a su tipo concreto.
class ScalarVisitor {
public:
    virtual ~ScalarVisitor() = default;
    virtual void visit(const Constant&) = 0;
    virtual void visit(const Parameter&) = 0;
    virtual void visit(const Fixing&) = 0;
    virtual void visit(const Current&) = 0;
    virtual void visit(const Add&) = 0;
    virtual void visit(const Sub&) = 0;
    virtual void visit(const Mul&) = 0;
    virtual void visit(const Div&) = 0;
    virtual void visit(const Neg&) = 0;
    virtual void visit(const Abs&) = 0;
    virtual void visit(const Exp&) = 0;
    virtual void visit(const Log&) = 0;
    virtual void visit(const Pow&) = 0;
    virtual void visit(const Min&) = 0;
    virtual void visit(const Max&) = 0;
    virtual void visit(const Clamp&) = 0;
    virtual void visit(const Average&) = 0;
    virtual void visit(const RunningMin&) = 0;
    virtual void visit(const RunningMax&) = 0;
    virtual void visit(const EventTime&) = 0;
    virtual void visit(const EventValue&) = 0;
    virtual void visit(const DiscountFactor&) = 0;
    virtual void visit(const FxConversion&) = 0;
};

// Forward declarations de los nodos de `Predicate` (PLAN_PRODUCTS.md §3.3).
class Greater;
class Less;
class GreaterEqual;
class LessEqual;
class Eq;
class All;
class Any;
class Not;
class Between;
class EventOccurred;
class Before;
class After;

class PredicateVisitor {
public:
    virtual ~PredicateVisitor() = default;
    virtual void visit(const Greater&) = 0;
    virtual void visit(const Less&) = 0;
    virtual void visit(const GreaterEqual&) = 0;
    virtual void visit(const LessEqual&) = 0;
    virtual void visit(const Eq&) = 0;
    virtual void visit(const All&) = 0;
    virtual void visit(const Any&) = 0;
    virtual void visit(const Not&) = 0;
    virtual void visit(const Between&) = 0;
    virtual void visit(const EventOccurred&) = 0;
    virtual void visit(const Before&) = 0;
    virtual void visit(const After&) = 0;
};

// Forward declarations de los nodos de `Contract` (PLAN_PRODUCTS.md §3.4). `Exercise` se
// declara ya (ADR-P0-04) aunque `ScenarioEvaluator` no lo soporte hasta Fase 9.
class Zero;
class Cashflow;
class Give;
class Both;
class Scale;
class If;
class When;
class Trigger;
class Exercise;

class ContractVisitor {
public:
    virtual ~ContractVisitor() = default;
    virtual void visit(const Zero&) = 0;
    virtual void visit(const Cashflow&) = 0;
    virtual void visit(const Give&) = 0;
    virtual void visit(const Both&) = 0;
    virtual void visit(const Scale&) = 0;
    virtual void visit(const If&) = 0;
    virtual void visit(const When&) = 0;
    virtual void visit(const Trigger&) = 0;
    virtual void visit(const Exercise&) = 0;
};

} // namespace payoff
} // namespace engine
