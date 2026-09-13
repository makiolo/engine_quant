#include "engine/payoff/json_parser.hpp"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/predicate.hpp"

namespace engine {
namespace payoff {

namespace {

namespace dom = simdjson::dom;

struct ParserState {
    std::size_t node_count = 0;
    std::size_t depth = 0;
    const ParseLimits* limits = nullptr;
};

// RAII de profundidad (mismo espiritu que kMaxDepth/depth_ de ValidationVisitor, pero como
// contador propio del parser: no depende de ningun limite interno de simdjson).
struct DepthGuard {
    ParserState& state;

    DepthGuard(ParserState& s, const NodePath& path) : state(s) {
        if (++state.depth > state.limits->max_depth) {
            --state.depth;
            throw ParseError("profundidad maxima del arbol excedida", path);
        }
    }

    ~DepthGuard() { --state.depth; }
};

void check_node_budget(ParserState& state, const NodePath& path) {
    if (++state.node_count > state.limits->max_nodes) {
        throw ParseError("limite de " + std::to_string(state.limits->max_nodes) + " nodos excedido", path);
    }
}

// Rechaza cualquier key del objeto que no este en `allowed` (PLAN_PRODUCTS.md §7.2: "rechazo
// de campos desconocidos por defecto", independiente de lo que ya imponga el schema JSON). Las
// keys REQUERIDAS ausentes no se comprueban aqui: la lectura posterior de ese campo via
// simdjson lanza NO_SUCH_FIELD, traducido a ParseError por el catch de cada build_*.
void check_known_fields(const dom::object& obj, std::initializer_list<const char*> allowed, const NodePath& path) {
    for (dom::key_value_pair field : obj) {
        std::string_view key = field.key;
        bool found = false;
        for (const char* a : allowed) {
            if (key == a) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw ParseError("campo desconocido: '" + std::string(key) + "'", path);
        }
    }
}

std::vector<TimePoint> read_time_array(dom::element element) {
    dom::array arr = element;
    std::vector<TimePoint> result;
    for (double t : arr) result.push_back(TimePoint{t});
    return result;
}

ScalarExprPtr build_scalar(dom::element element, const NodePath& path, ParserState& state);
PredicatePtr build_predicate(dom::element element, const NodePath& path, ParserState& state);
ContractPtr build_contract(dom::element element, const NodePath& path, ParserState& state);

ScalarExprPtr build_scalar(dom::element element, const NodePath& path, ParserState& state) {
    DepthGuard depth_guard(state, path);
    check_node_budget(state, path);
    try {
        dom::object obj = element;
        std::string_view type = obj["type"];

        if (type == "constant") {
            check_known_fields(obj, {"type", "value"}, path);
            double value = obj["value"];
            return constant(value);
        }
        if (type == "parameter") {
            check_known_fields(obj, {"type", "name"}, path);
            std::string_view name = obj["name"];
            return parameter(std::string(name));
        }
        if (type == "fixing") {
            check_known_fields(obj, {"type", "observable", "time"}, path);
            std::string_view observable = obj["observable"];
            double time = obj["time"];
            return fixing(ObservableId{std::string(observable)}, TimePoint{time});
        }
        if (type == "current") {
            check_known_fields(obj, {"type", "observable"}, path);
            std::string_view observable = obj["observable"];
            return current(ObservableId{std::string(observable)});
        }
        if (type == "add" || type == "sub" || type == "mul" || type == "div" || type == "min" || type == "max") {
            check_known_fields(obj, {"type", "left", "right"}, path);
            ScalarExprPtr left = build_scalar(obj["left"], path.child("left"), state);
            ScalarExprPtr right = build_scalar(obj["right"], path.child("right"), state);
            if (type == "add") return add(left, right);
            if (type == "sub") return sub(left, right);
            if (type == "mul") return mul(left, right);
            if (type == "div") return div(left, right);
            if (type == "min") return minimum(left, right);
            return maximum(left, right);
        }
        if (type == "neg" || type == "abs" || type == "exp" || type == "log") {
            check_known_fields(obj, {"type", "operand"}, path);
            ScalarExprPtr operand = build_scalar(obj["operand"], path.child("operand"), state);
            if (type == "neg") return neg(operand);
            if (type == "abs") return abs(operand);
            if (type == "exp") return exp(operand);
            return log(operand);
        }
        if (type == "pow") {
            check_known_fields(obj, {"type", "base", "exponent"}, path);
            ScalarExprPtr base = build_scalar(obj["base"], path.child("base"), state);
            ScalarExprPtr exponent = build_scalar(obj["exponent"], path.child("exponent"), state);
            return pow(base, exponent);
        }
        if (type == "clamp") {
            check_known_fields(obj, {"type", "value", "low", "high"}, path);
            ScalarExprPtr value = build_scalar(obj["value"], path.child("value"), state);
            ScalarExprPtr low = build_scalar(obj["low"], path.child("low"), state);
            ScalarExprPtr high = build_scalar(obj["high"], path.child("high"), state);
            return clamp(value, low, high);
        }
        if (type == "average") {
            check_known_fields(obj, {"type", "observable", "schedule", "weights"}, path);
            std::string_view observable = obj["observable"];
            std::vector<TimePoint> schedule = read_time_array(obj["schedule"]);
            dom::array weights_arr = obj["weights"];
            std::vector<double> weights;
            for (double w : weights_arr) weights.push_back(w);
            return average(ObservableId{std::string(observable)}, std::move(schedule), std::move(weights));
        }
        if (type == "running_min" || type == "running_max") {
            check_known_fields(obj, {"type", "observable"}, path);
            std::string_view observable = obj["observable"];
            return type == "running_min" ? running_min(ObservableId{std::string(observable)})
                                          : running_max(ObservableId{std::string(observable)});
        }
        if (type == "event_time") {
            check_known_fields(obj, {"type", "event"}, path);
            std::string_view event = obj["event"];
            return event_time(EventId{std::string(event)});
        }
        if (type == "event_value") {
            check_known_fields(obj, {"type", "event", "observable"}, path);
            std::string_view event = obj["event"];
            std::string_view observable = obj["observable"];
            return event_value(EventId{std::string(event)}, ObservableId{std::string(observable)});
        }
        if (type == "discount_factor") {
            check_known_fields(obj, {"type", "curve", "from", "to"}, path);
            std::string_view curve = obj["curve"];
            double from = obj["from"];
            double to = obj["to"];
            return discount_factor(CurveId{std::string(curve)}, TimePoint{from}, TimePoint{to});
        }
        if (type == "fx_conversion") {
            check_known_fields(obj, {"type", "from_currency", "to_currency", "time"}, path);
            std::string_view from_currency = obj["from_currency"];
            std::string_view to_currency = obj["to_currency"];
            double time = obj["time"];
            return fx_conversion(Currency{std::string(from_currency)}, Currency{std::string(to_currency)}, TimePoint{time});
        }
        throw ParseError("tipo de nodo escalar desconocido: '" + std::string(type) + "'", path);
    } catch (const simdjson::simdjson_error& e) {
        throw ParseError(e.what(), path);
    }
}

PredicatePtr build_predicate(dom::element element, const NodePath& path, ParserState& state) {
    DepthGuard depth_guard(state, path);
    check_node_budget(state, path);
    try {
        dom::object obj = element;
        std::string_view type = obj["type"];

        if (type == "greater" || type == "less" || type == "greater_equal" || type == "less_equal") {
            check_known_fields(obj, {"type", "left", "right"}, path);
            ScalarExprPtr left = build_scalar(obj["left"], path.child("left"), state);
            ScalarExprPtr right = build_scalar(obj["right"], path.child("right"), state);
            if (type == "greater") return greater(left, right);
            if (type == "less") return less(left, right);
            if (type == "greater_equal") return greater_equal(left, right);
            return less_equal(left, right);
        }
        if (type == "eq") {
            check_known_fields(obj, {"type", "left", "right", "tolerance"}, path);
            ScalarExprPtr left = build_scalar(obj["left"], path.child("left"), state);
            ScalarExprPtr right = build_scalar(obj["right"], path.child("right"), state);
            double tolerance = obj["tolerance"];
            return eq(left, right, tolerance);
        }
        if (type == "all" || type == "any") {
            check_known_fields(obj, {"type", "operands"}, path);
            dom::array arr = obj["operands"];
            std::vector<PredicatePtr> operands;
            std::size_t idx = 0;
            for (dom::element item : arr) {
                operands.push_back(build_predicate(item, path.child(idx, "operands"), state));
                ++idx;
            }
            return type == "all" ? all_of(std::move(operands)) : any_of(std::move(operands));
        }
        if (type == "not") {
            check_known_fields(obj, {"type", "operand"}, path);
            return negate(build_predicate(obj["operand"], path.child("operand"), state));
        }
        if (type == "between") {
            check_known_fields(obj, {"type", "value", "low", "high", "low_inclusive", "high_inclusive"}, path);
            ScalarExprPtr value = build_scalar(obj["value"], path.child("value"), state);
            ScalarExprPtr low = build_scalar(obj["low"], path.child("low"), state);
            ScalarExprPtr high = build_scalar(obj["high"], path.child("high"), state);
            bool low_inclusive = obj["low_inclusive"];
            bool high_inclusive = obj["high_inclusive"];
            return between(value, low, high, low_inclusive, high_inclusive);
        }
        if (type == "event_occurred") {
            check_known_fields(obj, {"type", "event"}, path);
            std::string_view event = obj["event"];
            return event_occurred(EventId{std::string(event)});
        }
        if (type == "before" || type == "after") {
            check_known_fields(obj, {"type", "time"}, path);
            double time = obj["time"];
            return type == "before" ? before(TimePoint{time}) : after(TimePoint{time});
        }
        throw ParseError("tipo de predicado desconocido: '" + std::string(type) + "'", path);
    } catch (const simdjson::simdjson_error& e) {
        throw ParseError(e.what(), path);
    }
}

ContractPtr build_contract(dom::element element, const NodePath& path, ParserState& state) {
    DepthGuard depth_guard(state, path);
    check_node_budget(state, path);
    try {
        dom::object obj = element;
        std::string_view type = obj["type"];

        if (type == "zero") {
            check_known_fields(obj, {"type"}, path);
            return zero();
        }
        if (type == "cashflow") {
            check_known_fields(obj, {"type", "currency", "amount"}, path);
            std::string_view currency = obj["currency"];
            ScalarExprPtr amount = build_scalar(obj["amount"], path.child("amount"), state);
            return cashflow(Currency{std::string(currency)}, amount);
        }
        if (type == "give") {
            check_known_fields(obj, {"type", "child"}, path);
            return give(build_contract(obj["child"], path.child("child"), state));
        }
        if (type == "both") {
            check_known_fields(obj, {"type", "children"}, path);
            dom::array children_arr = obj["children"];
            std::vector<ContractPtr> children;
            std::size_t idx = 0;
            for (dom::element item : children_arr) {
                children.push_back(build_contract(item, path.child(idx, "children"), state));
                ++idx;
            }
            return both(std::move(children));
        }
        if (type == "scale") {
            check_known_fields(obj, {"type", "factor", "child"}, path);
            ScalarExprPtr factor = build_scalar(obj["factor"], path.child("factor"), state);
            ContractPtr child = build_contract(obj["child"], path.child("child"), state);
            return scale(factor, child);
        }
        if (type == "if") {
            check_known_fields(obj, {"type", "condition", "if_true", "if_false"}, path);
            PredicatePtr condition = build_predicate(obj["condition"], path.child("condition"), state);
            ContractPtr if_true = build_contract(obj["if_true"], path.child("if_true"), state);
            ContractPtr if_false = build_contract(obj["if_false"], path.child("if_false"), state);
            return if_(condition, if_true, if_false);
        }
        if (type == "when") {
            check_known_fields(obj, {"type", "time", "child"}, path);
            double time = obj["time"];
            ContractPtr child = build_contract(obj["child"], path.child("child"), state);
            return when(TimePoint{time}, child);
        }
        if (type == "trigger") {
            check_known_fields(
                obj,
                {"type", "id", "monitoring_times", "condition", "monitoring", "settlement", "priority", "latch",
                 "on_hit", "on_miss"},
                path
            );
            std::string_view id = obj["id"];
            std::vector<TimePoint> monitoring_times = read_time_array(obj["monitoring_times"]);
            PredicatePtr condition = build_predicate(obj["condition"], path.child("condition"), state);

            std::string_view monitoring_sv = obj["monitoring"];
            Monitoring monitoring;
            if (monitoring_sv == "discrete") {
                monitoring = Monitoring::Discrete;
            } else if (monitoring_sv == "continuous_approximation") {
                monitoring = Monitoring::ContinuousApproximation;
            } else {
                throw ParseError("valor de 'monitoring' desconocido: '" + std::string(monitoring_sv) + "'", path.child("monitoring"));
            }

            std::string_view settlement_sv = obj["settlement"];
            Settlement settlement;
            if (settlement_sv == "at_hit") {
                settlement = Settlement::AtHit;
            } else if (settlement_sv == "at_scheduled_payment") {
                settlement = Settlement::AtScheduledPayment;
            } else {
                throw ParseError("valor de 'settlement' desconocido: '" + std::string(settlement_sv) + "'", path.child("settlement"));
            }

            double priority = obj["priority"];
            bool latch = obj["latch"];
            ContractPtr on_hit = build_contract(obj["on_hit"], path.child("on_hit"), state);
            ContractPtr on_miss = build_contract(obj["on_miss"], path.child("on_miss"), state);

            TriggerSpec spec{
                EventId{std::string(id)}, std::move(monitoring_times), condition, monitoring, settlement,
                static_cast<int>(priority), latch
            };
            return trigger(std::move(spec), std::move(on_hit), std::move(on_miss));
        }
        if (type == "exercise") {
            check_known_fields(obj, {"type", "id", "dates", "exercise_value", "continuation"}, path);
            std::string_view id = obj["id"];
            std::vector<TimePoint> dates = read_time_array(obj["dates"]);
            ScalarExprPtr exercise_value = build_scalar(obj["exercise_value"], path.child("exercise_value"), state);
            ContractPtr continuation = build_contract(obj["continuation"], path.child("continuation"), state);
            return exercise(EventId{std::string(id)}, std::move(dates), std::move(exercise_value), std::move(continuation));
        }
        throw ParseError("tipo de contrato desconocido: '" + std::string(type) + "'", path);
    } catch (const simdjson::simdjson_error& e) {
        throw ParseError(e.what(), path);
    }
}

} // namespace

ParsedPayoffDocument parse_payoff_document(std::string_view json_text, const ParseLimits& limits) {
    if (json_text.size() > limits.max_bytes) {
        throw ParseError(
            "documento JSON supera el limite de " + std::to_string(limits.max_bytes) + " bytes", NodePath::root()
        );
    }

    dom::parser parser;
    dom::element doc;
    try {
        simdjson::padded_string padded(json_text.data(), json_text.size());
        doc = parser.parse(padded);
    } catch (const simdjson::simdjson_error& e) {
        throw ParseError(std::string("JSON malformado: ") + e.what(), NodePath::root());
    }

    ParserState state;
    state.limits = &limits;

    try {
        dom::object top = doc;
        check_known_fields(top, {"schema", "id", "contract"}, NodePath::root());

        std::string_view schema = top["schema"];
        if (schema != "engine.payoff/v1") {
            throw ParseError(
                "schema desconocido: '" + std::string(schema) + "' (se esperaba 'engine.payoff/v1')", NodePath::root()
            );
        }
        std::string_view id = top["id"];
        ContractPtr contract = build_contract(top["contract"], NodePath::root().child("contract"), state);
        return ParsedPayoffDocument{std::string(id), std::move(contract)};
    } catch (const simdjson::simdjson_error& e) {
        throw ParseError(e.what(), NodePath::root());
    }
}

} // namespace payoff
} // namespace engine
