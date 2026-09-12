#include "engine/payoff/validation_visitor.hpp"

#include <cmath>

namespace engine {
namespace payoff {

namespace {
constexpr int kMaxDepth = 1024;
}

std::vector<ValidationError> ValidationVisitor::validate(const ContractPtr& root) {
    errors_.clear();
    current_path_ = NodePath::root();
    cursor_active_ = false;
    depth_ = 0;
    defined_events_.clear();
    referenced_events_.clear();

    if (root) {
        root->accept(*this);
    } else {
        add_error("contrato raiz nulo");
    }

    for (const auto& [event_id, path] : referenced_events_) {
        if (defined_events_.find(event_id) == defined_events_.end()) {
            errors_.emplace_back(
                "referencia a EventId '" + event_id.value + "' sin Trigger/Exercise que lo defina", path
            );
        }
    }

    return errors_;
}

void ValidationVisitor::add_error(const std::string& message) { errors_.emplace_back(message, current_path_); }

void ValidationVisitor::check_time_finite(TimePoint t, const std::string& field) {
    if (!std::isfinite(t.year_fraction)) {
        errors_.emplace_back("fecha no finita en '" + field + "'", current_path_.child(field));
    }
}

void ValidationVisitor::check_schedule_ascending_and_finite(
    const std::vector<TimePoint>& schedule, const std::string& field
) {
    if (schedule.empty()) {
        errors_.emplace_back("'" + field + "' no puede estar vacio", current_path_.child(field));
        return;
    }
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        if (!std::isfinite(schedule[i].year_fraction)) {
            errors_.emplace_back(
                "fecha no finita en '" + field + "'", current_path_.child(i, field)
            );
        }
        if (i > 0 && !time_less(schedule[i - 1], schedule[i])) {
            errors_.emplace_back(
                "'" + field + "' debe estar estrictamente ordenado ascendente", current_path_.child(i, field)
            );
        }
    }
}

void ValidationVisitor::record_event_definition(const EventId& id) {
    auto [it, inserted] = defined_events_.emplace(id, true);
    (void)it;
    if (!inserted) {
        errors_.emplace_back("EventId '" + id.value + "' duplicado", current_path_);
    }
}

void ValidationVisitor::record_event_reference(const EventId& id) {
    referenced_events_.emplace_back(id, current_path_);
}

void ValidationVisitor::descend_scalar(const ScalarExprPtr& expr, const std::string& field) {
    if (!expr) {
        errors_.emplace_back("nodo nulo en '" + field + "'", current_path_.child(field));
        return;
    }
    ++depth_;
    if (depth_ > kMaxDepth) {
        errors_.emplace_back("profundidad maxima del arbol excedida", current_path_.child(field));
        --depth_;
        return;
    }
    NodePath saved_path = current_path_;
    current_path_ = current_path_.child(field);
    expr->accept(*this);
    current_path_ = saved_path;
    --depth_;
}

void ValidationVisitor::descend_predicate(const PredicatePtr& pred, const std::string& field) {
    if (!pred) {
        errors_.emplace_back("nodo nulo en '" + field + "'", current_path_.child(field));
        return;
    }
    ++depth_;
    if (depth_ > kMaxDepth) {
        errors_.emplace_back("profundidad maxima del arbol excedida", current_path_.child(field));
        --depth_;
        return;
    }
    NodePath saved_path = current_path_;
    current_path_ = current_path_.child(field);
    pred->accept(*this);
    current_path_ = saved_path;
    --depth_;
}

void ValidationVisitor::descend_contract(const ContractPtr& child, const std::string& field) {
    if (!child) {
        errors_.emplace_back("nodo nulo en '" + field + "'", current_path_.child(field));
        return;
    }
    ++depth_;
    if (depth_ > kMaxDepth) {
        errors_.emplace_back("profundidad maxima del arbol excedida", current_path_.child(field));
        --depth_;
        return;
    }
    NodePath saved_path = current_path_;
    current_path_ = current_path_.child(field);
    child->accept(*this);
    current_path_ = saved_path;
    --depth_;
}

void ValidationVisitor::descend_contract_indexed(
    const ContractPtr& child, std::size_t index, const std::string& field
) {
    if (!child) {
        errors_.emplace_back("nodo nulo en '" + field + "[" + std::to_string(index) + "]'", current_path_);
        return;
    }
    ++depth_;
    if (depth_ > kMaxDepth) {
        errors_.emplace_back("profundidad maxima del arbol excedida", current_path_);
        --depth_;
        return;
    }
    NodePath saved_path = current_path_;
    current_path_ = current_path_.child(index, field);
    child->accept(*this);
    current_path_ = saved_path;
    --depth_;
}

// ---- ScalarVisitor ----

void ValidationVisitor::visit(const Constant&) {}

void ValidationVisitor::visit(const Parameter&) {}

void ValidationVisitor::visit(const Fixing& node) { check_time_finite(node.time(), "time"); }

void ValidationVisitor::visit(const Current&) {
    if (!cursor_active_) {
        add_error("Current sin instante activo (falta un When/Trigger AtHit envolvente, ADR-P0-08)");
    }
}

void ValidationVisitor::visit(const Add& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Sub& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Mul& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Div& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Neg& node) { descend_scalar(node.operand(), "operand"); }

void ValidationVisitor::visit(const Abs& node) { descend_scalar(node.operand(), "operand"); }

void ValidationVisitor::visit(const Exp& node) { descend_scalar(node.operand(), "operand"); }

void ValidationVisitor::visit(const Log& node) { descend_scalar(node.operand(), "operand"); }

void ValidationVisitor::visit(const Pow& node) {
    descend_scalar(node.base(), "base");
    descend_scalar(node.exponent(), "exponent");
}

void ValidationVisitor::visit(const Min& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Max& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Clamp& node) {
    descend_scalar(node.value(), "value");
    descend_scalar(node.low(), "low");
    descend_scalar(node.high(), "high");
}

void ValidationVisitor::visit(const Average& node) {
    check_schedule_ascending_and_finite(node.schedule(), "schedule");
    if (node.schedule().size() != node.weights().size()) {
        add_error("'schedule' y 'weights' deben tener la misma longitud");
    }
}

void ValidationVisitor::visit(const RunningMin&) {
    if (!cursor_active_) {
        add_error("RunningMin sin instante activo (ADR-P0-08)");
    }
}

void ValidationVisitor::visit(const RunningMax&) {
    if (!cursor_active_) {
        add_error("RunningMax sin instante activo (ADR-P0-08)");
    }
}

void ValidationVisitor::visit(const EventTime& node) { record_event_reference(node.event()); }

void ValidationVisitor::visit(const EventValue& node) { record_event_reference(node.event()); }

void ValidationVisitor::visit(const DiscountFactor& node) {
    check_time_finite(node.from(), "from");
    check_time_finite(node.to(), "to");
}

void ValidationVisitor::visit(const FxConversion& node) {
    if (node.from_currency().code.empty()) add_error("moneda de origen vacia en FxConversion");
    if (node.to_currency().code.empty()) add_error("moneda de destino vacia en FxConversion");
    check_time_finite(node.time(), "time");
}

// ---- PredicateVisitor ----

void ValidationVisitor::visit(const Greater& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Less& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const GreaterEqual& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const LessEqual& node) {
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const Eq& node) {
    if (!(node.tolerance() >= 0.0) || !std::isfinite(node.tolerance())) {
        add_error("Eq requiere una tolerancia finita >= 0");
    }
    descend_scalar(node.left(), "left");
    descend_scalar(node.right(), "right");
}

void ValidationVisitor::visit(const All& node) {
    if (node.operands().empty()) add_error("All requiere al menos un operando");
    for (std::size_t i = 0; i < node.operands().size(); ++i) {
        NodePath saved_path = current_path_;
        current_path_ = current_path_.child(i, "operands");
        if (node.operands()[i]) node.operands()[i]->accept(*this);
        current_path_ = saved_path;
    }
}

void ValidationVisitor::visit(const Any& node) {
    if (node.operands().empty()) add_error("Any requiere al menos un operando");
    for (std::size_t i = 0; i < node.operands().size(); ++i) {
        NodePath saved_path = current_path_;
        current_path_ = current_path_.child(i, "operands");
        if (node.operands()[i]) node.operands()[i]->accept(*this);
        current_path_ = saved_path;
    }
}

void ValidationVisitor::visit(const Not& node) { descend_predicate(node.operand(), "operand"); }

void ValidationVisitor::visit(const Between& node) {
    descend_scalar(node.value(), "value");
    descend_scalar(node.low(), "low");
    descend_scalar(node.high(), "high");
}

void ValidationVisitor::visit(const EventOccurred& node) { record_event_reference(node.event()); }

void ValidationVisitor::visit(const Before& node) { check_time_finite(node.time(), "time"); }

void ValidationVisitor::visit(const After& node) { check_time_finite(node.time(), "time"); }

// ---- ContractVisitor ----

void ValidationVisitor::visit(const Zero&) {}

void ValidationVisitor::visit(const Cashflow& node) {
    if (node.currency().code.empty()) add_error("moneda vacia en Cashflow");
    if (!cursor_active_) {
        add_error("Cashflow sin instante activo (falta un When/Trigger AtHit envolvente, ADR-P0-08)");
    }
    descend_scalar(node.amount(), "amount");
}

void ValidationVisitor::visit(const Give& node) { descend_contract(node.child(), "child"); }

void ValidationVisitor::visit(const Both& node) {
    for (std::size_t i = 0; i < node.children().size(); ++i) {
        descend_contract_indexed(node.children()[i], i, "children");
    }
}

void ValidationVisitor::visit(const Scale& node) {
    descend_scalar(node.factor(), "factor");
    descend_contract(node.child(), "child");
}

void ValidationVisitor::visit(const If& node) {
    descend_predicate(node.condition(), "condition");
    descend_contract(node.if_true(), "if_true");
    descend_contract(node.if_false(), "if_false");
}

void ValidationVisitor::visit(const When& node) {
    check_time_finite(node.time(), "time");
    bool saved_cursor = cursor_active_;
    cursor_active_ = true;
    descend_contract(node.child(), "child");
    cursor_active_ = saved_cursor;
}

void ValidationVisitor::visit(const Trigger& node) {
    const TriggerSpec& spec = node.spec();
    record_event_definition(spec.id);
    check_schedule_ascending_and_finite(spec.monitoring_times, "monitoring_times");
    if (spec.monitoring == Monitoring::ContinuousApproximation) {
        add_error(
            "Monitoring::ContinuousApproximation requiere soporte de Brownian bridge (Fase 6, "
            "PLAN_PRODUCTS.md §4.2): no soportado por el evaluador determinista de Fases 1-2"
        );
    }

    bool saved_cursor = cursor_active_;
    cursor_active_ = true;
    descend_predicate(spec.condition, "condition");
    cursor_active_ = saved_cursor;

    cursor_active_ = (spec.settlement == Settlement::AtHit);
    descend_contract(node.on_hit(), "on_hit");

    cursor_active_ = false;
    descend_contract(node.on_miss(), "on_miss");

    cursor_active_ = saved_cursor;
}

void ValidationVisitor::visit(const Exercise& node) {
    record_event_definition(node.id());
    check_schedule_ascending_and_finite(node.dates(), "dates");
    descend_scalar(node.exercise_value(), "exercise_value");
    descend_contract(node.continuation(), "continuation");
}

} // namespace payoff
} // namespace engine
