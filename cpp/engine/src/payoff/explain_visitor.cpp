#include "engine/payoff/explain_visitor.hpp"

#include <sstream>

#include "engine/payoff/expression.hpp"
#include "engine/payoff/predicate.hpp"

namespace engine {
namespace payoff {

namespace {

// Impresor recursivo indentado, mínimo pero completo sobre las tres jerarquías (§5.1,
// "ExplainVisitor será una función de producto, no texto ensamblado dentro de cada medida").
class TreePrinter : public ScalarVisitor, public PredicateVisitor, public ContractVisitor {
public:
    std::string print_contract(const ContractPtr& root) {
        if (root) root->accept(*this); else write_line("<null>");
        return out_.str();
    }

private:
    void write_line(const std::string& text) {
        out_ << std::string(static_cast<std::size_t>(indent_) * 2, ' ') << text << "\n";
    }

    void print_scalar(const ScalarExprPtr& expr) {
        ++indent_;
        if (expr) expr->accept(*this); else write_line("<null>");
        --indent_;
    }

    void print_predicate(const PredicatePtr& pred) {
        ++indent_;
        if (pred) pred->accept(*this); else write_line("<null>");
        --indent_;
    }

    void print_contract_child(const ContractPtr& child, const char* label) {
        ++indent_;
        write_line(std::string(label) + ":");
        ++indent_;
        if (child) child->accept(*this); else write_line("<null>");
        --indent_;
        --indent_;
    }

    // ScalarVisitor
    void visit(const Constant& n) override { write_line("Constant(" + std::to_string(n.value()) + ")"); }
    void visit(const Parameter& n) override { write_line("Parameter(" + n.name() + ")"); }
    void visit(const Fixing& n) override {
        write_line("Fixing(" + n.observable().value + ", t=" + std::to_string(n.time().year_fraction) + ")");
    }
    void visit(const Current& n) override { write_line("Current(" + n.observable().value + ")"); }
    void visit(const Add& n) override { write_line("Add"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const Sub& n) override { write_line("Sub"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const Mul& n) override { write_line("Mul"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const Div& n) override { write_line("Div"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const Neg& n) override { write_line("Neg"); print_scalar(n.operand()); }
    void visit(const Abs& n) override { write_line("Abs"); print_scalar(n.operand()); }
    void visit(const Exp& n) override { write_line("Exp"); print_scalar(n.operand()); }
    void visit(const Log& n) override { write_line("Log"); print_scalar(n.operand()); }
    void visit(const Pow& n) override { write_line("Pow"); print_scalar(n.base()); print_scalar(n.exponent()); }
    void visit(const Min& n) override { write_line("Min"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const Max& n) override { write_line("Max"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const Clamp& n) override {
        write_line("Clamp");
        print_scalar(n.value());
        print_scalar(n.low());
        print_scalar(n.high());
    }
    void visit(const Average& n) override {
        write_line("Average(" + n.observable().value + ", n=" + std::to_string(n.schedule().size()) + ")");
    }
    void visit(const RunningMin& n) override { write_line("RunningMin(" + n.observable().value + ")"); }
    void visit(const RunningMax& n) override { write_line("RunningMax(" + n.observable().value + ")"); }
    void visit(const EventTime& n) override { write_line("EventTime(" + n.event().value + ")"); }
    void visit(const EventValue& n) override {
        write_line("EventValue(" + n.event().value + ", " + n.observable().value + ")");
    }
    void visit(const DiscountFactor& n) override {
        write_line(
            "DiscountFactor(" + n.curve().value + ", " + std::to_string(n.from().year_fraction) + "->" +
            std::to_string(n.to().year_fraction) + ")"
        );
    }
    void visit(const FxConversion& n) override {
        write_line(
            "FxConversion(" + n.from_currency().code + "->" + n.to_currency().code +
            ", t=" + std::to_string(n.time().year_fraction) + ")"
        );
    }

    // PredicateVisitor
    void visit(const Greater& n) override { write_line("Greater"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const Less& n) override { write_line("Less"); print_scalar(n.left()); print_scalar(n.right()); }
    void visit(const GreaterEqual& n) override {
        write_line("GreaterEqual");
        print_scalar(n.left());
        print_scalar(n.right());
    }
    void visit(const LessEqual& n) override {
        write_line("LessEqual");
        print_scalar(n.left());
        print_scalar(n.right());
    }
    void visit(const Eq& n) override {
        write_line("Eq(tolerance=" + std::to_string(n.tolerance()) + ")");
        print_scalar(n.left());
        print_scalar(n.right());
    }
    void visit(const All& n) override {
        write_line("All");
        for (const auto& operand : n.operands()) print_predicate(operand);
    }
    void visit(const Any& n) override {
        write_line("Any");
        for (const auto& operand : n.operands()) print_predicate(operand);
    }
    void visit(const Not& n) override { write_line("Not"); print_predicate(n.operand()); }
    void visit(const Between& n) override {
        write_line(
            std::string("Between(low_inclusive=") + (n.low_inclusive() ? "true" : "false") +
            ", high_inclusive=" + (n.high_inclusive() ? "true" : "false") + ")"
        );
        print_scalar(n.value());
        print_scalar(n.low());
        print_scalar(n.high());
    }
    void visit(const EventOccurred& n) override { write_line("EventOccurred(" + n.event().value + ")"); }
    void visit(const Before& n) override { write_line("Before(t=" + std::to_string(n.time().year_fraction) + ")"); }
    void visit(const After& n) override { write_line("After(t=" + std::to_string(n.time().year_fraction) + ")"); }

    // ContractVisitor
    void visit(const Zero&) override { write_line("Zero"); }
    void visit(const Cashflow& n) override {
        write_line("Cashflow(" + n.currency().code + ")");
        print_scalar(n.amount());
    }
    void visit(const Give& n) override {
        write_line("Give");
        ++indent_;
        if (n.child()) n.child()->accept(*this); else write_line("<null>");
        --indent_;
    }
    void visit(const Both& n) override {
        write_line("Both");
        for (const auto& child : n.children()) {
            ++indent_;
            if (child) child->accept(*this); else write_line("<null>");
            --indent_;
        }
    }
    void visit(const Scale& n) override {
        write_line("Scale");
        print_scalar(n.factor());
        ++indent_;
        if (n.child()) n.child()->accept(*this); else write_line("<null>");
        --indent_;
    }
    void visit(const If& n) override {
        write_line("If");
        print_contract_child(n.if_true(), "if_true");
        print_contract_child(n.if_false(), "if_false");
        write_line("condition:");
        print_predicate(n.condition());
    }
    void visit(const When& n) override {
        write_line("When(t=" + std::to_string(n.time().year_fraction) + ")");
        ++indent_;
        if (n.child()) n.child()->accept(*this); else write_line("<null>");
        --indent_;
    }
    void visit(const Trigger& n) override {
        write_line("Trigger(" + n.spec().id.value + ", priority=" + std::to_string(n.spec().priority) + ")");
        print_contract_child(n.on_hit(), "on_hit");
        print_contract_child(n.on_miss(), "on_miss");
    }
    void visit(const Exercise& n) override {
        write_line("Exercise(" + n.id().value + ")");
        print_contract_child(n.continuation(), "continuation");
    }

    std::ostringstream out_;
    int indent_ = 0;
};

} // namespace

std::string ExplainVisitor::explain_tree(const ContractPtr& root) const {
    TreePrinter printer;
    return printer.print_contract(root);
}

std::string ExplainVisitor::explain_ledger(const CashflowLedger& ledger) const {
    std::ostringstream out;
    for (const auto& entry : ledger) {
        out << "t=" << entry.payment_time.year_fraction << " " << entry.currency.code << " " << entry.amount;
        if (entry.source_event) out << " event=" << entry.source_event->value;
        out << " node=" << entry.source_node.value << "\n";
    }
    return out.str();
}

} // namespace payoff
} // namespace engine
