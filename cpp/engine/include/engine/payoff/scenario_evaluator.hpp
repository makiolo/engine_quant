#pragma once

#include "engine/payoff/contract.hpp"
#include "engine/payoff/evaluation_context.hpp"
#include "engine/payoff/ledger.hpp"

namespace engine {
namespace payoff {

// Interpreta el contrato sobre una única ruta conocida (PLAN_PRODUCTS.md §5.1, §5.2): no
// descuenta, no decide Q/P -- solo ejecuta el programa y produce el `CashflowLedger` pathwise.
// El pricer bajo Q/P (Fase 4+) llama a este evaluador y descuenta/convierte el resultado.
//
// Este commit (Fase 1) soporta los nodos sin estado (Zero/Cashflow/Give/Both/Scale/If/When).
// `Trigger` se añade en el commit siguiente (Fase 2, event store de dos pasadas, ADR-P0-08).
// `Exercise` permanece sin soporte hasta Fase 9 (ADR-P0-04): evaluarlo lanza
// `std::logic_error`, no `EvaluationError` (no es un fallo de datos de una ruta concreta, es
// una capacidad no implementada del motor).
class ScenarioEvaluator {
public:
    CashflowLedger evaluate(const ContractPtr& root, EvaluationContext& context) const;
};

} // namespace payoff
} // namespace engine
