#include "engine/payoff/payoff_product.hpp"

#include <stdexcept>
#include <utility>

#include "engine/payoff/canonical_visitor.hpp"
#include "engine/payoff/explain_visitor.hpp"
#include "engine/payoff/json_parser.hpp"
#include "engine/payoff/validation_visitor.hpp"

namespace engine {
namespace payoff {

PayoffProgram PayoffProduct::build_program(std::string id, ContractPtr contract) {
    auto errors = ValidationVisitor{}.validate(contract);
    if (!errors.empty()) {
        std::string message = "PayoffProduct: " + std::to_string(errors.size()) + " error(es) de validacion:";
        for (const auto& e : errors) {
            message += "\n  - ";
            message += e.what();
        }
        throw std::invalid_argument(message);
    }
    std::string hash = CanonicalVisitor::hash(id, contract);
    return PayoffProgram{std::move(id), std::move(contract), std::move(hash)};
}

PayoffProduct::PayoffProduct(std::string id, ContractPtr contract)
    : program_(build_program(std::move(id), std::move(contract))) {}

PayoffProduct::PayoffProduct(const Params& params) {
    const std::string& spec = get_string(params, "spec");
    ParsedPayoffDocument doc = parse_payoff_document(spec);
    program_ = build_program(std::move(doc.id), std::move(doc.contract));
}

std::string PayoffProduct::explain() const {
    std::string header = "Payoff '" + program_.id + "' (hash=" + program_.canonical_hash + ")\n";
    return header + ExplainVisitor{}.explain_tree(program_.contract);
}

std::vector<std::string> validate_payoff_spec(const std::string& spec_json) {
    ParsedPayoffDocument doc;
    try {
        doc = parse_payoff_document(spec_json);
    } catch (const std::exception& e) {
        return {e.what()};
    }
    auto errors = ValidationVisitor{}.validate(doc.contract);
    std::vector<std::string> messages;
    messages.reserve(errors.size());
    for (const auto& e : errors) {
        messages.emplace_back(e.what());
    }
    return messages;
}

std::string explain_payoff_spec(const std::string& spec_json) {
    ParsedPayoffDocument doc = parse_payoff_document(spec_json);
    PayoffProduct product(std::move(doc.id), std::move(doc.contract));
    return product.explain();
}

} // namespace payoff
} // namespace engine
