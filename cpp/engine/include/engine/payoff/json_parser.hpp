#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "engine/payoff/contract.hpp"

namespace engine {
namespace payoff {

// Límites de recursos comprobados antes de/durante el parseo (PLAN_PRODUCTS.md §7.2, §15
// "JSON hostil o enorme"). `max_depth` es un contador propio del constructor de AST, no
// depende de ningún límite interno de simdjson.
struct ParseLimits {
    std::size_t max_bytes = 1'000'000;
    std::size_t max_nodes = 100'000;
    std::size_t max_depth = 1024;
};

// Documento `engine.payoff/v1` ya parseado (PLAN_PRODUCTS.md §7.2).
struct ParsedPayoffDocument {
    std::string id;
    ContractPtr contract;
};

// Parsea un documento `engine.payoff/v1` (JSON) a un `ContractPtr` (§7.2). Lanza `ParseError`
// en el PRIMER problema encontrado (JSON malformado, `schema` ausente/desconocido, campo
// inesperado o de tipo incorrecto, límite de `limits` excedido) -- a diferencia de
// `ValidationVisitor`, que agrega todos los errores de un árbol ya construido. No ejecuta
// `ValidationVisitor`: eso sigue siendo responsabilidad del llamador (ver `PayoffProduct`).
ParsedPayoffDocument parse_payoff_document(std::string_view json_text, const ParseLimits& limits = {});

} // namespace payoff
} // namespace engine
