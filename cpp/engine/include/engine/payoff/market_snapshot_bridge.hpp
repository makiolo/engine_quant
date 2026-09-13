#pragma once

#include "engine/market.hpp"
#include "engine/payoff/contract.hpp"
#include "engine/payoff/measures.hpp"

namespace engine {
namespace payoff {

// Puente mínimo `MarketSnapshot` -> motor de payoff (PLAN_PRODUCTS.md §9.5, versión acotada de
// Fase 8): NO es el `MarketDataView` genérico que describe §9.5 (una interfaz que soporte
// múltiples curvas/observables arbitrarios sobre cualquier `MarketSnapshot`) -- cubre
// exactamente el caso ya resuelto hoy por `PresentValueMeasure`/`Dv01Measure` para
// `IrSwapProduct`: un schedule determinista de cashflows en una ÚNICA moneda, descontado con la
// ÚNICA curva que trae `MarketSnapshot` (`market.discount_factor(t)`). Es lo que necesita
// `templates::irs_swap` y cualquier payoff custom de la misma forma (sin `Fixing`/`Current`/
// `Average`/`RunningMin`/`RunningMax` -- ningún observable de mercado que `MarketSnapshot` no
// modele -- y sin más de una moneda).
//
// Cómo se decide la moneda/curva: NO se piden como parámetro. El contrato no lleva un `CurveId`
// (`Cashflow` solo tiene `currency` -- la curva es una decisión de `DiscountingPolicy` en tiempo
// de valoración, nunca del propio AST, §3.5). Se evalúa el ledger una vez sobre un mercado
// vacío para descubrir qué moneda(s) usa; si hay más de una, es `std::invalid_argument`
// explícito (el caso multi-moneda -- p.ej. `templates::fx_forward` -- necesita las curvas
// doméstica/extranjera/basis de `PLAN_FXFORWARD.md` §3.2, que `MarketSnapshot` todavía no
// tiene). Esa única moneda se descuenta con la única curva de `market`.
//
// Un payoff fuera de este alcance (más de una moneda, o cualquier observable que
// `MarketSnapshot` no modele) lanza `EvaluationError`/`std::invalid_argument` al llamar estas
// funciones -- explícito, nunca un resultado silenciosamente incorrecto (§3.1).
ValuationResult present_value_from_market_snapshot(const ContractPtr& root, const MarketSnapshot& market);

// Bump-and-reval paralelo de tipo cero sobre la única curva de `market` (mismo bump que
// `compute_dv01`/`Dv01Measure::evaluate` en measure.cpp).
double bump_and_reval_from_market_snapshot(const ContractPtr& root, const MarketSnapshot& market, double zero_rate_bump);

} // namespace payoff
} // namespace engine
