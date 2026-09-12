#pragma once

#include <set>
#include <unordered_map>

#include "engine/payoff/contract.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/ids.hpp"
#include "engine/payoff/predicate.hpp"
#include "engine/payoff/visitor.hpp"

namespace engine {
namespace payoff {

// Observables, curvas, fechas de fixing, monedas y eventos que un contrato requiere para
// evaluarse (PLAN_PRODUCTS.md §5.1). Se calcula antes de reservar rutas/paths o lanzar un
// kernel (§6, "el DependencyVisitor y las capacidades se comparan antes..."), no durante la
// evaluación. No incluye `ModelCapabilities` (Fase 4+): eso se compara aparte contra este
// reporte, no forma parte de él.
struct DependencyReport {
    std::set<ObservableId> observables;
    std::set<CurveId> curves;
    std::set<TimePoint, TimePointLess> fixing_dates;
    std::set<Currency> currencies;
    std::set<EventId> events;

    // Observables que `EventValue(event, observable)` captura para cada evento, en cualquier
    // punto del árbol (§4.1 "sus valores capturados"). Usado por `ScenarioEvaluator` para
    // saber qué capturar en `EventState::captured_values` al resolver un `Trigger` (Fase 2),
    // reutilizando este único recorrido en vez de un escáner ad-hoc por evento.
    std::unordered_map<EventId, std::set<ObservableId>> event_value_observables;
};

class DependencyVisitor : private ScalarVisitor, private PredicateVisitor, private ContractVisitor {
public:
    DependencyReport analyze(const ContractPtr& root);

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

    void visit_scalar(const ScalarExprPtr& expr);
    void visit_predicate(const PredicatePtr& pred);
    void visit_contract(const ContractPtr& node);

    DependencyReport report_;
};

} // namespace payoff
} // namespace engine
