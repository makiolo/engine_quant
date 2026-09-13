#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "engine/market.hpp"
#include "engine/payoff/contract.hpp"
#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {
namespace specialization {

// Swap fijo-flotante reconocido en la forma EXACTA que produce `templates::irs_swap`
// (irs_templates.hpp): `Both(floating_leg, Give(fixed_leg))` con cashflows desnudos bajo `When`
// y `amount` como un único `Constant` (PLAN_PRODUCTS.md §9.3, §5.1 "SpecializationVisitor").
//
// `fixed_payments[i].second` es `notional * fixed_rate * accruals[i]` YA MULTIPLICADO -- el AST
// no preserva `fixed_rate`/`accruals` por separado (`irs_swap` los pliega en un único `Constant`
// por `Cashflow`). Eso basta para PV/DV01 (funciones lineales de esos productos, ver
// `specialized_present_value`) pero NO para especializar `ExposureProfile`/`UnilateralCVA` hacia
// Hull-White: `irs_hull_white_exposure_profile` (measure.cpp) necesita `fixed_rate`/`accruals`
// por separado para generar cashflows bajo cada trayectoria simulada, no solo su producto.
// Especializar esas dos medidas queda pendiente -- requeriría o preservar la factorización en
// el AST (`Mul(Mul(notional, fixed_rate), accrual_i)` en vez de un único `Constant`) o
// transportar el `IrsSwapSpec` original junto al `ContractPtr`; ninguna de las dos decidida
// todavía. PLAN_PRODUCTS.md Fase 8 solo exige "reconoce el patrón compatible y llama a los
// kernels actuales de Hull-White/exposición" -- este incremento cubre la parte de esos kernels
// que ya es determinista/curva-only (PV, DV01); la parte Monte Carlo no tiene todavía puente
// AST->Hull-White en absoluto (el único modelo conectado al motor de payoff hoy es GBM, Fases
// 5-7), así que "especializarla" no sería recortar trabajo sobre un camino existente, sería
// construirlo desde cero -- fuera de alcance de este incremento.
struct RecognizedIrsSwap {
    Currency currency;
    double notional = 0.0;
    TimePoint start{0.0};
    TimePoint end{0.0};
    std::vector<std::pair<TimePoint, double>> fixed_payments;
};

// Reconoce si `root` tiene exactamente la forma que produce `templates::irs_swap`. No reconoce
// árboles equivalentes construidos de otra forma (p.ej. hijos de `Both` en otro orden, o
// cashflows envueltos en nodos adicionales) -- reconocimiento de un patrón canónico concreto,
// no normalización/CSE (eso es Fase 11, PLAN_PRODUCTS.md §11).
std::optional<RecognizedIrsSwap> recognize_irs_swap(const ContractPtr& root);

// PV especializado (PLAN_PRODUCTS.md §9.3): idéntico en resultado a `npv_from_market`
// (measure.cpp) -- `PV = notional*(DF(start)-DF(end)) - sum(amount_i * DF(T_i))` -- calculado
// directamente sobre el swap reconocido, sin pasar por `MarketPath`/`ScenarioEvaluator` ni
// reconstruir un `IrSwapProduct`.
double specialized_present_value(const RecognizedIrsSwap& irs, const MarketSnapshot& market);

// DV01 especializado: mismo bump paralelo de tipo cero que `compute_dv01` (measure.cpp),
// `PV(curva bumpeada en bump) - PV(curva base)`.
double specialized_dv01(const RecognizedIrsSwap& irs, const MarketSnapshot& market, double bump);

} // namespace specialization
} // namespace payoff
} // namespace engine
