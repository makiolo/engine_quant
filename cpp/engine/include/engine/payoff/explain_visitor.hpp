#pragma once

#include <string>

#include "engine/payoff/contract.hpp"
#include "engine/payoff/ledger.hpp"

namespace engine {
namespace payoff {

// Representación legible del árbol y del ledger, con procedencia por nodo/evento
// (PLAN_PRODUCTS.md §5.1, §14). Versión mínima (Fases 1-2): suficiente para depurar
// validación/evaluación; la versión completa (hash, medida Q/P, malla efectiva, seed) depende
// de `CanonicalVisitor` (Fase 3) y de conceptos de medida (Fase 4+) que no existen todavía.
class ExplainVisitor {
public:
    std::string explain_tree(const ContractPtr& root) const;
    std::string explain_ledger(const CashflowLedger& ledger) const;
};

} // namespace payoff
} // namespace engine
