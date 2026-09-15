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
//
// `valuation_time` (PLAN_GREEKS.md §7.2/Fase 5, aditivo, default `0.0`): registra el discount
// factor de cada cashflow como "valor en `valuation_time`" (`discount_factor(t) /
// discount_factor(valuation_time)`, misma curva absoluta) en vez de "valor en 0" -- Theta puro
// (§7.1), delegado en `present_value(..., TimePoint{valuation_time})`. Un cashflow cuyo
// `payment_time < valuation_time` (ya "pagado" respecto del nuevo valuation_time) NO se registra
// -- `present_value` lo encuentra ausente y lanza explícito (§7.4: sin `FixingStore` para
// reemplazarlo por su valor histórico, nunca se aproxima en silencio).
ValuationResult present_value_from_market_snapshot(
    const ContractPtr& root, const MarketSnapshot& market, double valuation_time = 0.0
);

// Bump-and-reval paralelo de tipo cero sobre la única curva de `market` (mismo bump que
// `compute_dv01`/`Dv01Measure::evaluate` en measure.cpp).
double bump_and_reval_from_market_snapshot(const ContractPtr& root, const MarketSnapshot& market, double zero_rate_bump);

// Bump-and-reval de un ÚNICO pillar de la curva de `market` (PLAN_GREEKS.md §11 Fase 3: DV01
// bucketed de un PayoffProduct, antes explícitamente no soportado -- "Fase 8" pendiente en
// `Dv01Measure::evaluate`). Misma convención que `bump_and_reval_from_market_snapshot` (bump
// unidireccional, no diferencia central): `bumped - base`, MISMO tipo de cambio que la versión
// paralela.
double bump_and_reval_pillar_from_market_snapshot(
    const ContractPtr& root, const MarketSnapshot& market, std::size_t pillar_index, double zero_rate_bump
);

} // namespace payoff
} // namespace engine
