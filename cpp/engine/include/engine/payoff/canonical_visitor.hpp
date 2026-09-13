#pragma once

#include <string>

#include "engine/payoff/contract.hpp"

namespace engine {
namespace payoff {

// Serialización canónica del AST (PLAN_PRODUCTS.md §7.2): claves en un orden FIJO por tipo de
// nodo (el mismo orden que el array "required" de cada `$def` en
// `docs/schema/engine.payoff/v1.schema.json`), floats con `std::to_chars` (forma "shortest
// round-trip"), sin espacios opcionales -- para que el mismo AST produzca siempre el mismo
// texto y, por tanto, el mismo hash (§14 "hash y versión del payoff").
class CanonicalVisitor {
public:
    static std::string to_json(const std::string& id, const ContractPtr& contract);

    // FNV-1a de 64 bits (ADR de Fase 3: no criptográfico, suficiente para cache/dedup/
    // trazabilidad) sobre `to_json(id, contract)`. Hex minúsculas, 16 caracteres.
    static std::string hash(const std::string& id, const ContractPtr& contract);
};

} // namespace payoff
} // namespace engine
