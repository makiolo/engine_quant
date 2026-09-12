#pragma once

#include <memory>
#include <vector>

#include "engine/payoff/expression.hpp"
#include "engine/payoff/ids.hpp"
#include "engine/payoff/predicate.hpp"
#include "engine/payoff/visitor.hpp"

namespace engine {
namespace payoff {

// Nodo base del ciclo de vida de un producto (PLAN_PRODUCTS.md §3.4). Doble dispatch
// (ADR-P0-06); sin estado mutable -- el estado de ruta vive en `RuntimeState`
// (event_state.hpp), nunca en el árbol.
class Contract {
public:
    explicit Contract(NodeId id) : node_id_(id) {}
    virtual ~Contract() = default;
    virtual void accept(ContractVisitor& visitor) const = 0;
    NodeId node_id() const { return node_id_; }

private:
    NodeId node_id_;
};

using ContractPtr = std::shared_ptr<const Contract>;

// No genera cashflows.
class Zero : public Contract {
public:
    Zero();
    void accept(ContractVisitor& visitor) const override;
};

// Genera un pago en el instante activo (ADR-P0-08: fijado por `When` o por `Trigger` en
// `on_hit` con `settlement == AtHit`). `amount` positivo = entrada de caja para el titular
// del AST raíz (ADR-P0-02).
class Cashflow : public Contract {
public:
    Cashflow(Currency currency, ScalarExprPtr amount);
    const Currency& currency() const { return currency_; }
    const ScalarExprPtr& amount() const { return amount_; }
    void accept(ContractVisitor& visitor) const override;

private:
    Currency currency_;
    ScalarExprPtr amount_;
};

// Invierte el signo de todos los cashflows del hijo (ADR-P0-02).
class Give : public Contract {
public:
    explicit Give(ContractPtr child);
    const ContractPtr& child() const { return child_; }
    void accept(ContractVisitor& visitor) const override;

private:
    ContractPtr child_;
};

// Posee todos los hijos; generaliza `And` binario (§3.4). Un `Both` sin hijos no tiene
// significado distinguible de `Zero` -- se rechaza en construcción (invariante trivial de
// forma permitida por ADR-P0-05, no una regla de negocio de `ValidationVisitor`).
class Both : public Contract {
public:
    explicit Both(std::vector<ContractPtr> children);
    const std::vector<ContractPtr>& children() const { return children_; }
    void accept(ContractVisitor& visitor) const override;

private:
    std::vector<ContractPtr> children_;
};

// Multiplica los cashflows del hijo por `factor`.
class Scale : public Contract {
public:
    Scale(ScalarExprPtr factor, ContractPtr child);
    const ScalarExprPtr& factor() const { return factor_; }
    const ContractPtr& child() const { return child_; }
    void accept(ContractVisitor& visitor) const override;

private:
    ScalarExprPtr factor_;
    ContractPtr child_;
};

// Elige una rama en el instante de evaluación según `condition` (§3.4). No debe confundirse
// con `Exercise`: `If` decide por una condición conocida, `Exercise` es un derecho de decisión
// (ver nota en §3.4 sobre `Or`).
class If : public Contract {
public:
    If(PredicatePtr condition, ContractPtr if_true, ContractPtr if_false);
    const PredicatePtr& condition() const { return condition_; }
    const ContractPtr& if_true() const { return if_true_; }
    const ContractPtr& if_false() const { return if_false_; }
    void accept(ContractVisitor& visitor) const override;

private:
    PredicatePtr condition_;
    ContractPtr if_true_;
    ContractPtr if_false_;
};

// Evalúa el hijo en una fecha contractual: fija el cursor de "instante activo" (ADR-P0-08)
// consumido por `Cashflow`/`Current`/`RunningMin`/`RunningMax` dentro de `child`.
class When : public Contract {
public:
    When(TimePoint time, ContractPtr child);
    TimePoint time() const { return time_; }
    const ContractPtr& child() const { return child_; }
    void accept(ContractVisitor& visitor) const override;

private:
    TimePoint time_;
    ContractPtr child_;
};

// Monitorización de un `Trigger`: discreta (solo los puntos de `monitoring_times`) o
// aproximación continua (requiere que el modelo declare soporte de Brownian bridge --
// Fase 6; el evaluador determinista de Fases 1-2 rechaza `ContinuousApproximation`).
enum class Monitoring { Discrete, ContinuousApproximation };

// Cuándo se paga el `Cashflow` "desnudo" de `on_hit`/`on_miss` (ADR-P0-10): en el instante del
// primer hit, o en la fecha ya programada por el propio subárbol (si éste trae su `When`).
enum class Settlement { AtHit, AtScheduledPayment };

// Especificación de un evento path-dependent persistente (§4.1). El desempate de dos triggers
// simultáneos usa `priority` y, a igualdad, el orden canónico de `EventId` (ADR-P0-03).
struct TriggerSpec {
    EventId id;
    std::vector<TimePoint> monitoring_times;
    PredicatePtr condition;
    Monitoring monitoring;
    Settlement settlement;
    int priority;
    bool latch;
};

// Evento path-dependent persistente (§4.1, §4.2 barreras, §4.3 TP/SL). El AST solo declara el
// evento; la resolución de `EventState` (primer hit, valores capturados) la hace
// `ScenarioEvaluator` en tiempo de evaluación (ADR-P0-08), nunca el propio nodo.
class Trigger : public Contract {
public:
    Trigger(TriggerSpec spec, ContractPtr on_hit, ContractPtr on_miss);
    const TriggerSpec& spec() const { return spec_; }
    const ContractPtr& on_hit() const { return on_hit_; }
    const ContractPtr& on_miss() const { return on_miss_; }
    void accept(ContractVisitor& visitor) const override;

private:
    TriggerSpec spec_;
    ContractPtr on_hit_;
    ContractPtr on_miss_;
};

// Derecho de decisión americano/bermuda (§3.4, §10). Declarado ya en Fase 1 (ADR-P0-04) con
// una forma mínima; la política de ejercicio real (Longstaff-Schwartz, holder/issuer) se
// define en Fase 9. `ScenarioEvaluator` rechaza este nodo hasta entonces.
class Exercise : public Contract {
public:
    Exercise(EventId id, std::vector<TimePoint> dates, ScalarExprPtr exercise_value, ContractPtr continuation);
    const EventId& id() const { return id_; }
    const std::vector<TimePoint>& dates() const { return dates_; }
    const ScalarExprPtr& exercise_value() const { return exercise_value_; }
    const ContractPtr& continuation() const { return continuation_; }
    void accept(ContractVisitor& visitor) const override;

private:
    EventId id_;
    std::vector<TimePoint> dates_;
    ScalarExprPtr exercise_value_;
    ContractPtr continuation_;
};

// Builders libres (PLAN_PRODUCTS.md §7.1).
ContractPtr zero();
ContractPtr cashflow(Currency currency, ScalarExprPtr amount);
ContractPtr give(ContractPtr child);
ContractPtr both(std::vector<ContractPtr> children);
ContractPtr scale(ScalarExprPtr factor, ContractPtr child);
ContractPtr if_(PredicatePtr condition, ContractPtr if_true, ContractPtr if_false);
ContractPtr when(TimePoint time, ContractPtr child);
ContractPtr trigger(TriggerSpec spec, ContractPtr on_hit, ContractPtr on_miss);
ContractPtr exercise(EventId id, std::vector<TimePoint> dates, ScalarExprPtr exercise_value, ContractPtr continuation);

} // namespace payoff
} // namespace engine
