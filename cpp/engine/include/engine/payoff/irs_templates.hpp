#pragma once

#include <vector>

#include "engine/payoff/contract.hpp"
#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {
namespace templates {

// Especificación de un IRS fijo-flotante, single-curve (PLAN_PRODUCTS.md §9.3, Fase 8). Mismo
// patrón que `TpSlSpec` (tp_sl.hpp) para plantillas con más de ~5 parámetros. `fixed_rate` debe
// ser explícito: el AST no soporta el sentinel PAR de `IrSwapProduct::use_par_rate()` --
// resolverlo con la curva de mercado antes de construir el spec (§9.3: "se resuelve al
// construir el trade... o se representa como parámetro derivado explícito antes de congelar el
// contrato"). `payment_times`/`accruals` deben tener la misma longitud.
struct IrsSwapSpec {
    Currency currency;
    double notional;
    double fixed_rate;
    TimePoint start;
    std::vector<TimePoint> payment_times;
    std::vector<double> accruals;
};

// IRS fijo-flotante, single-curve (§9.3): cashflows reales en cada fecha; el discount factor lo
// aplica el visitor de valoración (`discount_and_convert`, measures.cpp) contra
// `DiscountingPolicy`/`MarketPath`, no el propio contrato -- consistente con §3.5 ("el visitor
// de valoración decide cómo descontar... no se agregan monedas automáticamente"). Por eso este
// template no recibe ni un `CurveId`: la curva de descuento es una decisión del llamante al
// valorar, no del contrato.
//
//   pata fija:     Both(When(T_i, Cashflow(ccy,  notional * fixed_rate * accruals[i])))
//   pata flotante: Both(When(start, Cashflow(ccy, notional)),
//                        When(T_last, Cashflow(ccy, -notional)))     (réplica single-curve)
//   AST          = Both(floating_leg, Give(fixed_leg))
//
// La pata fija entra como `Give` porque el titular la paga (ADR-P0-02, consistente con
// `notional() > 0` ⇒ posición pagadora y `NPV = flotante − fijo` en `measure.cpp`). La pata
// flotante single-curve se replica intercambiando el nocional en `start`/`T_last` (§3.2 "réplica
// equivalente documentada"): PV(flotante) = N*(DF(start) − DF(T_last)), idéntico a
// `npv_from_market` en `measure.cpp` una vez el visitor de valoración descuenta cada cashflow.
//
// `spec.payment_times`/`spec.accruals` de distinta longitud es `std::invalid_argument`: invariante
// de forma de la llamada (ADR-P0-05), no una regla de negocio del AST resultante. Ausencia de
// discount factor para cualquiera de las fechas en el `MarketPath` usado al valorar es error de
// evaluación, nunca de construcción.
ContractPtr irs_swap(const IrsSwapSpec& spec);

} // namespace templates
} // namespace payoff
} // namespace engine
