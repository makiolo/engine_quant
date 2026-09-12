#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/payoff/contract.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/predicate.hpp"
#include "engine/payoff/visitor.hpp"

namespace engine {
namespace payoff {

// Recorre un árbol completo agregando TODOS los errores encontrados con su `NodePath`, nunca
// se detiene en el primero (PLAN_PRODUCTS.md §3.1, §7.1, ADR-P0-05). Comprueba: monedas
// vacías, fechas no finitas, schedules desordenados, `EventId` duplicados o con referencias
// colgantes, límite de profundidad (defensa adicional, ADR-P0-07), rechazo de
// `Monitoring::ContinuousApproximation` (sin soporte de Brownian bridge hasta Fase 6, §4.2) y
// la regla de "instante activo" de `Cashflow`/`Current`/`RunningMin`/`RunningMax`
// (ADR-P0-08). No tiene acceso a datos de mercado: lo que depende de una ruta concreta es
// responsabilidad de `ScenarioEvaluator` (`EvaluationError`, no `ValidationError`).
class ValidationVisitor : private ScalarVisitor, private PredicateVisitor, private ContractVisitor {
public:
    std::vector<ValidationError> validate(const ContractPtr& root);

private:
    // ScalarVisitor
    void visit(const Constant&) override;
    void visit(const Parameter&) override;
    void visit(const Fixing&) override;
    void visit(const Current&) override;
    void visit(const Add&) override;
    void visit(const Sub&) override;
    void visit(const Mul&) override;
    void visit(const Div&) override;
    void visit(const Neg&) override;
    void visit(const Abs&) override;
    void visit(const Exp&) override;
    void visit(const Log&) override;
    void visit(const Pow&) override;
    void visit(const Min&) override;
    void visit(const Max&) override;
    void visit(const Clamp&) override;
    void visit(const Average&) override;
    void visit(const RunningMin&) override;
    void visit(const RunningMax&) override;
    void visit(const EventTime&) override;
    void visit(const EventValue&) override;
    void visit(const DiscountFactor&) override;
    void visit(const FxConversion&) override;

    // PredicateVisitor
    void visit(const Greater&) override;
    void visit(const Less&) override;
    void visit(const GreaterEqual&) override;
    void visit(const LessEqual&) override;
    void visit(const Eq&) override;
    void visit(const All&) override;
    void visit(const Any&) override;
    void visit(const Not&) override;
    void visit(const Between&) override;
    void visit(const EventOccurred&) override;
    void visit(const Before&) override;
    void visit(const After&) override;

    // ContractVisitor
    void visit(const Zero&) override;
    void visit(const Cashflow&) override;
    void visit(const Give&) override;
    void visit(const Both&) override;
    void visit(const Scale&) override;
    void visit(const If&) override;
    void visit(const When&) override;
    void visit(const Trigger&) override;
    void visit(const Exercise&) override;

    void descend_scalar(const ScalarExprPtr& expr, const std::string& field);
    void descend_predicate(const PredicatePtr& pred, const std::string& field);
    void descend_contract(const ContractPtr& child, const std::string& field);
    void descend_contract_indexed(const ContractPtr& child, std::size_t index, const std::string& field);

    void check_time_finite(TimePoint t, const std::string& field);
    void check_schedule_ascending_and_finite(const std::vector<TimePoint>& schedule, const std::string& field);
    void record_event_definition(const EventId& id);
    void record_event_reference(const EventId& id);
    void add_error(const std::string& message);

    std::vector<ValidationError> errors_;
    NodePath current_path_ = NodePath::root();
    bool cursor_active_ = false;
    int depth_ = 0;
    std::unordered_map<EventId, bool> defined_events_;
    std::vector<std::pair<EventId, NodePath>> referenced_events_;
};

} // namespace payoff
} // namespace engine
