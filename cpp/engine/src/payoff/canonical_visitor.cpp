#include "engine/payoff/canonical_visitor.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/predicate.hpp"

namespace engine {
namespace payoff {

namespace {

std::uint64_t fnv1a64(std::string_view data) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Compartida entre `Writer::write_string` y `CanonicalVisitor::to_json` (para el campo "id"
// del documento, fuera de la jerarquia de nodos que recorre `Writer`).
void append_json_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// Recorre las tres jerarquías emitiendo JSON canónico en `out_` (PLAN_PRODUCTS.md §7.2): orden
// de claves fijo por tipo de nodo, sin espacios opcionales, floats con `std::to_chars`.
class Writer : private ScalarVisitor, private PredicateVisitor, private ContractVisitor {
public:
    explicit Writer(std::string& out) : out_(out) {}

    void write(const ScalarExprPtr& expr) {
        if (!expr) throw ValidationError("nodo escalar nulo", NodePath::root());
        expr->accept(*this);
    }
    void write(const PredicatePtr& pred) {
        if (!pred) throw ValidationError("nodo de predicado nulo", NodePath::root());
        pred->accept(*this);
    }
    void write(const ContractPtr& node) {
        if (!node) throw ValidationError("nodo de contrato nulo", NodePath::root());
        node->accept(*this);
    }

private:
    void write_double(double value) {
        if (!std::isfinite(value)) {
            throw ValidationError("valor no finito en la serializacion canonica", NodePath::root());
        }
        if (value == 0.0) value = 0.0; // normaliza -0.0 a 0.0 (hash estable independiente del origen)
        char buf[32];
        auto result = std::to_chars(buf, buf + sizeof(buf), value);
        out_.append(buf, result.ptr);
    }

    void write_string(const std::string& s) { append_json_string(out_, s); }

    void write_bool(bool b) { out_ += b ? "true" : "false"; }

    void write_field_name(const char* name) {
        out_ += '"';
        out_ += name;
        out_ += "\":";
    }

    void write_time_array(const std::vector<TimePoint>& times) {
        out_ += '[';
        for (std::size_t i = 0; i < times.size(); ++i) {
            if (i > 0) out_ += ',';
            write_double(times[i].year_fraction);
        }
        out_ += ']';
    }

    // ---- ScalarVisitor ----

    void visit(const Constant& n) override {
        out_ += R"({"type":"constant","value":)";
        write_double(n.value());
        out_ += '}';
    }
    void visit(const Parameter& n) override {
        out_ += R"({"type":"parameter","name":)";
        write_string(n.name());
        out_ += '}';
    }
    void visit(const Fixing& n) override {
        out_ += R"({"type":"fixing","observable":)";
        write_string(n.observable().value);
        out_ += R"(,"time":)";
        write_double(n.time().year_fraction);
        out_ += '}';
    }
    void visit(const Current& n) override {
        out_ += R"({"type":"current","observable":)";
        write_string(n.observable().value);
        out_ += '}';
    }
    void visit(const Add& n) override { write_binary("add", n.left(), n.right()); }
    void visit(const Sub& n) override { write_binary("sub", n.left(), n.right()); }
    void visit(const Mul& n) override { write_binary("mul", n.left(), n.right()); }
    void visit(const Div& n) override { write_binary("div", n.left(), n.right()); }
    void visit(const Neg& n) override { write_unary("neg", n.operand()); }
    void visit(const Abs& n) override { write_unary("abs", n.operand()); }
    void visit(const Exp& n) override { write_unary("exp", n.operand()); }
    void visit(const Log& n) override { write_unary("log", n.operand()); }
    void visit(const Pow& n) override {
        out_ += R"({"type":"pow","base":)";
        write(n.base());
        out_ += R"(,"exponent":)";
        write(n.exponent());
        out_ += '}';
    }
    void visit(const Min& n) override { write_binary("min", n.left(), n.right()); }
    void visit(const Max& n) override { write_binary("max", n.left(), n.right()); }
    void visit(const Clamp& n) override {
        out_ += R"({"type":"clamp","value":)";
        write(n.value());
        out_ += R"(,"low":)";
        write(n.low());
        out_ += R"(,"high":)";
        write(n.high());
        out_ += '}';
    }
    void visit(const Average& n) override {
        out_ += R"({"type":"average","observable":)";
        write_string(n.observable().value);
        out_ += R"(,"schedule":)";
        write_time_array(n.schedule());
        out_ += R"(,"weights":[)";
        for (std::size_t i = 0; i < n.weights().size(); ++i) {
            if (i > 0) out_ += ',';
            write_double(n.weights()[i]);
        }
        out_ += "]}";
    }
    void visit(const RunningMin& n) override {
        out_ += R"({"type":"running_min","observable":)";
        write_string(n.observable().value);
        out_ += '}';
    }
    void visit(const RunningMax& n) override {
        out_ += R"({"type":"running_max","observable":)";
        write_string(n.observable().value);
        out_ += '}';
    }
    void visit(const EventTime& n) override {
        out_ += R"({"type":"event_time","event":)";
        write_string(n.event().value);
        out_ += '}';
    }
    void visit(const EventValue& n) override {
        out_ += R"({"type":"event_value","event":)";
        write_string(n.event().value);
        out_ += R"(,"observable":)";
        write_string(n.observable().value);
        out_ += '}';
    }
    void visit(const DiscountFactor& n) override {
        out_ += R"({"type":"discount_factor","curve":)";
        write_string(n.curve().value);
        out_ += R"(,"from":)";
        write_double(n.from().year_fraction);
        out_ += R"(,"to":)";
        write_double(n.to().year_fraction);
        out_ += '}';
    }
    void visit(const FxConversion& n) override {
        out_ += R"({"type":"fx_conversion","from_currency":)";
        write_string(n.from_currency().code);
        out_ += R"(,"to_currency":)";
        write_string(n.to_currency().code);
        out_ += R"(,"time":)";
        write_double(n.time().year_fraction);
        out_ += '}';
    }

    void write_binary(const char* type, const ScalarExprPtr& left, const ScalarExprPtr& right) {
        out_ += "{\"type\":\"";
        out_ += type;
        out_ += "\",\"left\":";
        write(left);
        out_ += R"(,"right":)";
        write(right);
        out_ += '}';
    }
    void write_unary(const char* type, const ScalarExprPtr& operand) {
        out_ += "{\"type\":\"";
        out_ += type;
        out_ += "\",\"operand\":";
        write(operand);
        out_ += '}';
    }

    // ---- PredicateVisitor ----

    void visit(const Greater& n) override { write_predicate_binary("greater", n.left(), n.right()); }
    void visit(const Less& n) override { write_predicate_binary("less", n.left(), n.right()); }
    void visit(const GreaterEqual& n) override { write_predicate_binary("greater_equal", n.left(), n.right()); }
    void visit(const LessEqual& n) override { write_predicate_binary("less_equal", n.left(), n.right()); }
    void visit(const Eq& n) override {
        out_ += R"({"type":"eq","left":)";
        write(n.left());
        out_ += R"(,"right":)";
        write(n.right());
        out_ += R"(,"tolerance":)";
        write_double(n.tolerance());
        out_ += '}';
    }
    void visit(const All& n) override { write_predicate_list("all", n.operands()); }
    void visit(const Any& n) override { write_predicate_list("any", n.operands()); }
    void visit(const Not& n) override {
        out_ += R"({"type":"not","operand":)";
        write(n.operand());
        out_ += '}';
    }
    void visit(const Between& n) override {
        out_ += R"({"type":"between","value":)";
        write(n.value());
        out_ += R"(,"low":)";
        write(n.low());
        out_ += R"(,"high":)";
        write(n.high());
        out_ += R"(,"low_inclusive":)";
        write_bool(n.low_inclusive());
        out_ += R"(,"high_inclusive":)";
        write_bool(n.high_inclusive());
        out_ += '}';
    }
    void visit(const EventOccurred& n) override {
        out_ += R"({"type":"event_occurred","event":)";
        write_string(n.event().value);
        out_ += '}';
    }
    void visit(const Before& n) override {
        out_ += R"({"type":"before","time":)";
        write_double(n.time().year_fraction);
        out_ += '}';
    }
    void visit(const After& n) override {
        out_ += R"({"type":"after","time":)";
        write_double(n.time().year_fraction);
        out_ += '}';
    }

    void write_predicate_binary(const char* type, const ScalarExprPtr& left, const ScalarExprPtr& right) {
        out_ += "{\"type\":\"";
        out_ += type;
        out_ += "\",\"left\":";
        write(left);
        out_ += R"(,"right":)";
        write(right);
        out_ += '}';
    }
    void write_predicate_list(const char* type, const std::vector<PredicatePtr>& operands) {
        out_ += "{\"type\":\"";
        out_ += type;
        out_ += "\",\"operands\":[";
        for (std::size_t i = 0; i < operands.size(); ++i) {
            if (i > 0) out_ += ',';
            write(operands[i]);
        }
        out_ += "]}";
    }

    // ---- ContractVisitor ----

    void visit(const Zero&) override { out_ += R"({"type":"zero"})"; }
    void visit(const Cashflow& n) override {
        out_ += R"({"type":"cashflow","currency":)";
        write_string(n.currency().code);
        out_ += R"(,"amount":)";
        write(n.amount());
        out_ += '}';
    }
    void visit(const Give& n) override {
        out_ += R"({"type":"give","child":)";
        write(n.child());
        out_ += '}';
    }
    void visit(const Both& n) override {
        out_ += R"({"type":"both","children":[)";
        for (std::size_t i = 0; i < n.children().size(); ++i) {
            if (i > 0) out_ += ',';
            write(n.children()[i]);
        }
        out_ += "]}";
    }
    void visit(const Scale& n) override {
        out_ += R"({"type":"scale","factor":)";
        write(n.factor());
        out_ += R"(,"child":)";
        write(n.child());
        out_ += '}';
    }
    void visit(const If& n) override {
        out_ += R"({"type":"if","condition":)";
        write(n.condition());
        out_ += R"(,"if_true":)";
        write(n.if_true());
        out_ += R"(,"if_false":)";
        write(n.if_false());
        out_ += '}';
    }
    void visit(const When& n) override {
        out_ += R"({"type":"when","time":)";
        write_double(n.time().year_fraction);
        out_ += R"(,"child":)";
        write(n.child());
        out_ += '}';
    }
    void visit(const Trigger& n) override {
        const TriggerSpec& spec = n.spec();
        out_ += R"({"type":"trigger","id":)";
        write_string(spec.id.value);
        out_ += R"(,"monitoring_times":)";
        write_time_array(spec.monitoring_times);
        out_ += R"(,"condition":)";
        write(spec.condition);
        out_ += R"(,"monitoring":)";
        write_string(spec.monitoring == Monitoring::Discrete ? "discrete" : "continuous_approximation");
        out_ += R"(,"settlement":)";
        write_string(spec.settlement == Settlement::AtHit ? "at_hit" : "at_scheduled_payment");
        out_ += R"(,"priority":)";
        write_double(static_cast<double>(spec.priority));
        out_ += R"(,"latch":)";
        write_bool(spec.latch);
        out_ += R"(,"on_hit":)";
        write(n.on_hit());
        out_ += R"(,"on_miss":)";
        write(n.on_miss());
        out_ += '}';
    }
    void visit(const Exercise& n) override {
        out_ += R"({"type":"exercise","id":)";
        write_string(n.id().value);
        out_ += R"(,"dates":)";
        write_time_array(n.dates());
        out_ += R"(,"exercise_value":)";
        write(n.exercise_value());
        out_ += R"(,"continuation":)";
        write(n.continuation());
        out_ += '}';
    }

    std::string& out_;
};

} // namespace

std::string CanonicalVisitor::to_json(const std::string& id, const ContractPtr& contract) {
    std::string out;
    out.reserve(256);
    out += R"({"schema":"engine.payoff/v1","id":)";
    append_json_string(out, id);
    out += R"(,"contract":)";
    Writer writer(out);
    writer.write(contract);
    out += '}';
    return out;
}

std::string CanonicalVisitor::hash(const std::string& id, const ContractPtr& contract) {
    std::string json = to_json(id, contract);
    std::uint64_t digest = fnv1a64(json);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(digest));
    return std::string(buf, 16);
}

} // namespace payoff
} // namespace engine
