#pragma once

#include <memory>
#include <vector>

#include "engine/payoff/expression.hpp"
#include "engine/payoff/ids.hpp"
#include "engine/payoff/visitor.hpp"

namespace engine {
namespace payoff {

// Nodo base de predicado (PLAN_PRODUCTS.md §3.3): composición booleana sobre `ScalarExpr` y
// estado de eventos, sin lógica de modelo, con doble dispatch (ADR-P0-06).
class Predicate {
public:
    explicit Predicate(NodeId id) : node_id_(id) {}
    virtual ~Predicate() = default;
    virtual void accept(PredicateVisitor& visitor) const = 0;
    NodeId node_id() const { return node_id_; }

private:
    NodeId node_id_;
};

using PredicatePtr = std::shared_ptr<const Predicate>;

class Greater : public Predicate {
public:
    Greater(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class Less : public Predicate {
public:
    Less(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class GreaterEqual : public Predicate {
public:
    GreaterEqual(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class LessEqual : public Predicate {
public:
    LessEqual(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

// Igualdad numérica exige tolerancia declarada (§3.3): nunca `==` de doubles como semántica
// contractual por defecto.
class Eq : public Predicate {
public:
    Eq(ScalarExprPtr left, ScalarExprPtr right, double tolerance);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    double tolerance() const { return tolerance_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
    double tolerance_;
};

// Cortocircuito definido (§3.3): `ScenarioEvaluator` evalúa `operands()` en orden y se detiene
// en el primer `false`.
class All : public Predicate {
public:
    explicit All(std::vector<PredicatePtr> operands);
    const std::vector<PredicatePtr>& operands() const { return operands_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    std::vector<PredicatePtr> operands_;
};

// Cortocircuito definido (§3.3): se detiene en el primer `true`.
class Any : public Predicate {
public:
    explicit Any(std::vector<PredicatePtr> operands);
    const std::vector<PredicatePtr>& operands() const { return operands_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    std::vector<PredicatePtr> operands_;
};

class Not : public Predicate {
public:
    explicit Not(PredicatePtr operand);
    const PredicatePtr& operand() const { return operand_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    PredicatePtr operand_;
};

// Rango con extremos inclusivos configurables (§3.3).
class Between : public Predicate {
public:
    Between(ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high, bool low_inclusive, bool high_inclusive);
    const ScalarExprPtr& value() const { return value_; }
    const ScalarExprPtr& low() const { return low_; }
    const ScalarExprPtr& high() const { return high_; }
    bool low_inclusive() const { return low_inclusive_; }
    bool high_inclusive() const { return high_inclusive_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    ScalarExprPtr value_;
    ScalarExprPtr low_;
    ScalarExprPtr high_;
    bool low_inclusive_;
    bool high_inclusive_;
};

// Consulta el estado persistente de una ruta (`EventState::occurred`, §4.1), no un valor
// escalar -- por eso vive en `Predicate`, no en `ScalarExpr`.
class EventOccurred : public Predicate {
public:
    explicit EventOccurred(EventId event);
    const EventId& event() const { return event_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    EventId event_;
};

// Condición temporal explícita sobre el cursor de evaluación actual (§3.3).
class Before : public Predicate {
public:
    explicit Before(TimePoint time);
    TimePoint time() const { return time_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    TimePoint time_;
};

class After : public Predicate {
public:
    explicit After(TimePoint time);
    TimePoint time() const { return time_; }
    void accept(PredicateVisitor& visitor) const override;

private:
    TimePoint time_;
};

// Builders libres (PLAN_PRODUCTS.md §7.1). `negate` en vez de `not`: en C++ estándar `not` es
// un token alternativo del operador `!` (<ciso646>), no un identificador disponible.
PredicatePtr greater(ScalarExprPtr left, ScalarExprPtr right);
PredicatePtr less(ScalarExprPtr left, ScalarExprPtr right);
PredicatePtr greater_equal(ScalarExprPtr left, ScalarExprPtr right);
PredicatePtr less_equal(ScalarExprPtr left, ScalarExprPtr right);
PredicatePtr eq(ScalarExprPtr left, ScalarExprPtr right, double tolerance);
PredicatePtr all_of(std::vector<PredicatePtr> operands);
PredicatePtr any_of(std::vector<PredicatePtr> operands);
PredicatePtr negate(PredicatePtr operand);
PredicatePtr between(
    ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high, bool low_inclusive, bool high_inclusive
);
PredicatePtr event_occurred(EventId event);
PredicatePtr before(TimePoint time);
PredicatePtr after(TimePoint time);

} // namespace payoff
} // namespace engine
