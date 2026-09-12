#pragma once

#include <optional>
#include <vector>

#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {

// Entrada de la salida pathwise (PLAN_PRODUCTS.md §3.5). Renombrada respecto al nodo AST
// `Cashflow` (contract.hpp) para evitar la colisión de nombres en C++: el documento usa
// "cashflow" tanto para el nodo del árbol como para la entrada del ledger.
struct LedgerEntry {
    TimePoint payment_time;
    Currency currency;
    double amount;
    std::optional<EventId> source_event;
    NodeId source_node;
};

// No se agregan monedas automáticamente (§3.5): el visitor de valoración decide cómo
// descontar/convertir; `ScenarioEvaluator` (Fases 1-2) solo produce el ledger.
using CashflowLedger = std::vector<LedgerEntry>;

} // namespace payoff
} // namespace engine
