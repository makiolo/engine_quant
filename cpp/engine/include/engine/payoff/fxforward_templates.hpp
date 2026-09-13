#pragma once

#include "engine/payoff/contract.hpp"
#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {
namespace templates {

// Especificación de un FX forward multi-moneda (PLAN_PRODUCTS.md §9.4, Fase 8; fórmulas de
// referencia en PLAN_FXFORWARD.md §2.1-§2.2). `buy_foreign=true` (default): el titular recibe
// `notional_foreign` en `foreign_currency` y paga `notional_foreign * forward_rate` en
// `domestic_currency`; `buy_foreign=false` invierte el signo de ambas patas. `forward_rate`
// (`K`, unidades de doméstica por unidad de extranjera) debe ser explícito -- el AST no
// resuelve el sentinel PAR de `PLAN_FXFORWARD.md` §2.2 (`K_par = F_mkt(T)`); calcularlo con la
// curva/spot de mercado antes de construir el spec, mismo criterio que `IrsSwapSpec::fixed_rate`
// (irs_templates.hpp).
struct FxForwardSpec {
    Currency foreign_currency;
    Currency domestic_currency;
    double notional_foreign;
    double forward_rate;
    TimePoint maturity;
    bool buy_foreign = true;
};

// FX forward multi-moneda (§9.4): dos cashflows en la misma fecha, cada uno en su propia
// moneda -- "primera prueba de que no hace falta una clase core nueva" (§9.4). El visitor de
// valoración (`discount_and_convert`, measures.cpp) descuenta cada pata con la curva que le
// corresponda vía `DiscountingPolicy` y convierte la extranjera a la moneda de reporting vía
// `MarketPath::fx_rate` -- el template no necesita `CurveId` ni tipo de cambio, igual que
// `irs_swap` no los necesita (la política de descuento/conversión es decisión del llamante al
// valorar, no del contrato, §3.5).
//
//   AST = When(maturity, Both(Cashflow(foreign, sign * notional_foreign),
//                              Cashflow(domestic, -sign * notional_foreign * forward_rate)))
//
// donde `sign = +1` si `buy_foreign`, `-1` si no (PLAN_FXFORWARD.md §2.1-§2.2).
ContractPtr fx_forward(const FxForwardSpec& spec);

} // namespace templates
} // namespace payoff
} // namespace engine
