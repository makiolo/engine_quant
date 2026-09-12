#include "engine/payoff/scenario_evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "engine/payoff/expression.hpp"
#include "engine/payoff/predicate.hpp"

namespace engine {
namespace payoff {

namespace {

// Precedencia de ADR-P0-08: el fixing histórico gana sobre el simulado/futuro para el mismo
// (observable, time).
double lookup_observable(EvaluationContext& context, const ObservableId& observable, TimePoint time, const NodePath& path) {
    if (context.historical_fixings.has(observable, time)) {
        return context.historical_fixings.get(observable, time, path);
    }
    return context.path.fixing(observable, time, path);
}

// Evalúa un `ScalarExpr` a `double` sobre una ruta conocida (§5.1, §5.2). `cursor` es el
// "instante activo" ambiente (ADR-P0-08): ausente si no hay ninguno vigente en este punto del
// árbol -- `Current`/`RunningMin`/`RunningMax` lo requieren y fallan si está ausente
// (`ValidationVisitor` ya rechaza estructuralmente los casos detectables antes de llegar aquí,
// pero el evaluador vuelve a comprobarlo: no todo árbol pasado a `evaluate()` ha sido validado).
class ScalarEvalVisitor : public ScalarVisitor {
public:
    ScalarEvalVisitor(EvaluationContext& context, std::optional<TimePoint> cursor)
        : context_(context), cursor_(cursor) {}

    double evaluate(const ScalarExprPtr& expr, NodePath path) {
        path_ = std::move(path);
        if (!expr) throw EvaluationError("nodo escalar nulo", path_);
        expr->accept(*this);
        return result_;
    }

private:
    double eval_child(const ScalarExprPtr& expr, const std::string& field) {
        ScalarEvalVisitor child(context_, cursor_);
        return child.evaluate(expr, path_.child(field));
    }

    void visit(const Constant& node) override { result_ = node.value(); }

    void visit(const Parameter&) override {
        throw EvaluationError(
            "Parameter no resuelto: ScenarioEvaluator requiere valores concretos (Constant); la "
            "sustitucion de parametros es responsabilidad de las plantillas/builders de mas alto nivel",
            path_
        );
    }

    void visit(const Fixing& node) override {
        result_ = lookup_observable(context_, node.observable(), node.time(), path_);
    }

    void visit(const Current& node) override {
        if (!cursor_) throw EvaluationError("Current sin instante activo", path_);
        result_ = lookup_observable(context_, node.observable(), *cursor_, path_);
    }

    void visit(const Add& node) override { result_ = eval_child(node.left(), "left") + eval_child(node.right(), "right"); }
    void visit(const Sub& node) override { result_ = eval_child(node.left(), "left") - eval_child(node.right(), "right"); }
    void visit(const Mul& node) override { result_ = eval_child(node.left(), "left") * eval_child(node.right(), "right"); }

    void visit(const Div& node) override {
        double left = eval_child(node.left(), "left");
        double right = eval_child(node.right(), "right");
        if (right == 0.0) throw EvaluationError("division por cero", path_.child("right"));
        result_ = left / right;
    }

    void visit(const Neg& node) override { result_ = -eval_child(node.operand(), "operand"); }
    void visit(const Abs& node) override { result_ = std::fabs(eval_child(node.operand(), "operand")); }
    void visit(const Exp& node) override { result_ = std::exp(eval_child(node.operand(), "operand")); }

    void visit(const Log& node) override {
        double operand = eval_child(node.operand(), "operand");
        if (!(operand > 0.0)) throw EvaluationError("log de un valor no positivo", path_.child("operand"));
        result_ = std::log(operand);
    }

    void visit(const Pow& node) override {
        result_ = std::pow(eval_child(node.base(), "base"), eval_child(node.exponent(), "exponent"));
    }

    void visit(const Min& node) override {
        result_ = std::min(eval_child(node.left(), "left"), eval_child(node.right(), "right"));
    }

    void visit(const Max& node) override {
        result_ = std::max(eval_child(node.left(), "left"), eval_child(node.right(), "right"));
    }

    void visit(const Clamp& node) override {
        double value = eval_child(node.value(), "value");
        double low = eval_child(node.low(), "low");
        double high = eval_child(node.high(), "high");
        result_ = std::clamp(value, low, high);
    }

    void visit(const Average& node) override {
        double sum = 0.0;
        const auto& schedule = node.schedule();
        const auto& weights = node.weights();
        if (schedule.size() != weights.size()) {
            throw EvaluationError("Average: 'schedule' y 'weights' de longitud distinta", path_);
        }
        for (std::size_t i = 0; i < schedule.size(); ++i) {
            double fixing_value = lookup_observable(context_, node.observable(), schedule[i], path_.child(i, "schedule"));
            sum += weights[i] * fixing_value;
        }
        result_ = sum;
    }

    void visit(const RunningMin& node) override {
        if (!cursor_) throw EvaluationError("RunningMin sin instante activo", path_);
        std::vector<double> values = context_.path.fixings_up_to(node.observable(), *cursor_);
        if (values.empty()) throw EvaluationError("RunningMin sin fixings observados hasta el instante actual", path_);
        result_ = *std::min_element(values.begin(), values.end());
    }

    void visit(const RunningMax& node) override {
        if (!cursor_) throw EvaluationError("RunningMax sin instante activo", path_);
        std::vector<double> values = context_.path.fixings_up_to(node.observable(), *cursor_);
        if (values.empty()) throw EvaluationError("RunningMax sin fixings observados hasta el instante actual", path_);
        result_ = *std::max_element(values.begin(), values.end());
    }

    void visit(const EventTime& node) override {
        const EventState* state = context_.state.find_event_state(node.event());
        if (state == nullptr || !state->occurred || !state->first_hit_time) {
            throw EvaluationError("EventTime: el evento '" + node.event().value + "' no ha ocurrido", path_);
        }
        result_ = state->first_hit_time->year_fraction;
    }

    void visit(const EventValue& node) override {
        const EventState* state = context_.state.find_event_state(node.event());
        if (state == nullptr || !state->occurred) {
            throw EvaluationError("EventValue: el evento '" + node.event().value + "' no ha ocurrido", path_);
        }
        auto it = state->captured_values.find(node.observable());
        if (it == state->captured_values.end()) {
            throw EvaluationError(
                "EventValue: '" + node.observable().value + "' no fue capturado por el evento '" +
                    node.event().value + "'",
                path_
            );
        }
        result_ = it->second;
    }

    void visit(const DiscountFactor& node) override {
        result_ = context_.path.discount_factor(node.curve(), node.from(), node.to(), path_);
    }

    void visit(const FxConversion& node) override {
        result_ = context_.path.fx_rate(node.from_currency(), node.to_currency(), node.time(), path_);
    }

    EvaluationContext& context_;
    std::optional<TimePoint> cursor_;
    NodePath path_ = NodePath::root();
    double result_ = 0.0;
};

double evaluate_scalar(
    const ScalarExprPtr& expr, EvaluationContext& context, std::optional<TimePoint> cursor, NodePath path
) {
    ScalarEvalVisitor visitor(context, cursor);
    return visitor.evaluate(expr, std::move(path));
}

// Evalúa un `Predicate` a `bool` sobre la misma ruta y el mismo cursor ambiente que el
// `ScalarExpr` (ADR-P0-08). Cortocircuito definido en `All`/`Any` (§3.3).
class PredicateEvalVisitor : public PredicateVisitor {
public:
    PredicateEvalVisitor(EvaluationContext& context, std::optional<TimePoint> cursor)
        : context_(context), cursor_(cursor) {}

    bool evaluate(const PredicatePtr& pred, NodePath path) {
        path_ = std::move(path);
        if (!pred) throw EvaluationError("nodo de predicado nulo", path_);
        pred->accept(*this);
        return result_;
    }

private:
    double eval_scalar(const ScalarExprPtr& expr, const std::string& field) {
        return evaluate_scalar(expr, context_, cursor_, path_.child(field));
    }

    bool eval_predicate(const PredicatePtr& pred, const std::string& field) {
        PredicateEvalVisitor child(context_, cursor_);
        return child.evaluate(pred, path_.child(field));
    }

    bool eval_predicate_indexed(const PredicatePtr& pred, std::size_t index, const std::string& field) {
        PredicateEvalVisitor child(context_, cursor_);
        return child.evaluate(pred, path_.child(index, field));
    }

    void visit(const Greater& node) override { result_ = eval_scalar(node.left(), "left") > eval_scalar(node.right(), "right"); }
    void visit(const Less& node) override { result_ = eval_scalar(node.left(), "left") < eval_scalar(node.right(), "right"); }
    void visit(const GreaterEqual& node) override {
        result_ = eval_scalar(node.left(), "left") >= eval_scalar(node.right(), "right");
    }
    void visit(const LessEqual& node) override {
        result_ = eval_scalar(node.left(), "left") <= eval_scalar(node.right(), "right");
    }

    void visit(const Eq& node) override {
        double left = eval_scalar(node.left(), "left");
        double right = eval_scalar(node.right(), "right");
        result_ = std::fabs(left - right) <= node.tolerance();
    }

    void visit(const All& node) override {
        for (std::size_t i = 0; i < node.operands().size(); ++i) {
            if (!eval_predicate_indexed(node.operands()[i], i, "operands")) {
                result_ = false;
                return;
            }
        }
        result_ = true;
    }

    void visit(const Any& node) override {
        for (std::size_t i = 0; i < node.operands().size(); ++i) {
            if (eval_predicate_indexed(node.operands()[i], i, "operands")) {
                result_ = true;
                return;
            }
        }
        result_ = false;
    }

    void visit(const Not& node) override { result_ = !eval_predicate(node.operand(), "operand"); }

    void visit(const Between& node) override {
        double value = eval_scalar(node.value(), "value");
        double low = eval_scalar(node.low(), "low");
        double high = eval_scalar(node.high(), "high");
        bool low_ok = node.low_inclusive() ? value >= low : value > low;
        bool high_ok = node.high_inclusive() ? value <= high : value < high;
        result_ = low_ok && high_ok;
    }

    void visit(const EventOccurred& node) override {
        const EventState* state = context_.state.find_event_state(node.event());
        result_ = state != nullptr && state->occurred;
    }

    void visit(const Before& node) override {
        if (!cursor_) throw EvaluationError("Before sin instante activo", path_);
        result_ = time_less(*cursor_, node.time());
    }

    void visit(const After& node) override {
        if (!cursor_) throw EvaluationError("After sin instante activo", path_);
        result_ = time_less(node.time(), *cursor_);
    }

    EvaluationContext& context_;
    std::optional<TimePoint> cursor_;
    NodePath path_ = NodePath::root();
    bool result_ = false;
};

bool evaluate_predicate(
    const PredicatePtr& pred, EvaluationContext& context, std::optional<TimePoint> cursor, NodePath path
) {
    PredicateEvalVisitor visitor(context, cursor);
    return visitor.evaluate(pred, std::move(path));
}

// Recorre `Contract` acumulando `CashflowLedger`. `scale_` es un multiplicador acumulado
// (ADR-P0-02): `Give` lo niega, `Scale` lo multiplica por su factor; `Cashflow` solo aplica el
// multiplicador vigente a su propio importe -- evita reprocesar el ledger después de emitirlo.
class ContractEvalVisitor : public ContractVisitor {
public:
    explicit ContractEvalVisitor(EvaluationContext& context) : context_(context) {}

    CashflowLedger evaluate(const ContractPtr& root) {
        if (!root) throw EvaluationError("contrato raiz nulo", path_);
        root->accept(*this);
        return std::move(ledger_);
    }

private:
    void descend(const ContractPtr& child, const std::string& field) {
        if (!child) throw EvaluationError("nodo nulo en '" + field + "'", path_.child(field));
        NodePath saved = path_;
        path_ = path_.child(field);
        child->accept(*this);
        path_ = saved;
    }

    void descend_indexed(const ContractPtr& child, std::size_t index, const std::string& field) {
        if (!child) {
            throw EvaluationError("nodo nulo en '" + field + "[" + std::to_string(index) + "]'", path_);
        }
        NodePath saved = path_;
        path_ = path_.child(index, field);
        child->accept(*this);
        path_ = saved;
    }

    void visit(const Zero&) override {}

    void visit(const Cashflow& node) override {
        if (!cursor_) throw EvaluationError("Cashflow sin instante activo", path_);
        double amount = evaluate_scalar(node.amount(), context_, cursor_, path_.child("amount"));
        ledger_.push_back(LedgerEntry{*cursor_, node.currency(), scale_ * amount, current_event_, node.node_id()});
    }

    void visit(const Give& node) override {
        double saved_scale = scale_;
        scale_ = -scale_;
        descend(node.child(), "child");
        scale_ = saved_scale;
    }

    void visit(const Both& node) override {
        for (std::size_t i = 0; i < node.children().size(); ++i) {
            descend_indexed(node.children()[i], i, "children");
        }
    }

    void visit(const Scale& node) override {
        double factor = evaluate_scalar(node.factor(), context_, cursor_, path_.child("factor"));
        double saved_scale = scale_;
        scale_ = scale_ * factor;
        descend(node.child(), "child");
        scale_ = saved_scale;
    }

    void visit(const If& node) override {
        bool condition = evaluate_predicate(node.condition(), context_, cursor_, path_.child("condition"));
        if (condition) {
            descend(node.if_true(), "if_true");
        } else {
            descend(node.if_false(), "if_false");
        }
    }

    void visit(const When& node) override {
        std::optional<TimePoint> saved_cursor = cursor_;
        cursor_ = node.time();
        descend(node.child(), "child");
        cursor_ = saved_cursor;
    }

    void visit(const Trigger&) override {
        throw EvaluationError("Trigger requiere Fase 2 (evaluador determinista sin estado en este commit)", path_);
    }

    void visit(const Exercise&) override {
        throw std::logic_error("Exercise no soportado hasta Fase 9 (PLAN_PRODUCTS.md §10, ADR-P0-04)");
    }

    EvaluationContext& context_;
    CashflowLedger ledger_;
    NodePath path_ = NodePath::root();
    std::optional<TimePoint> cursor_;
    double scale_ = 1.0;
    std::optional<EventId> current_event_;
};

} // namespace

CashflowLedger ScenarioEvaluator::evaluate(const ContractPtr& root, EvaluationContext& context) const {
    ContractEvalVisitor visitor(context);
    return visitor.evaluate(root);
}

} // namespace payoff
} // namespace engine
