#pragma once

#include <vector>

#include "engine/payoff/contract.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {
namespace templates {

// Métrica de cierre de posición (PLAN_PRODUCTS.md §4.3): v1 soporta las tres primeras de la
// lista del documento (precio del subyacente, retorno desde entrada, P&L monetario); el
// mark-to-market de un contrato hijo queda pospuesto (requiere valoración anidada/caché).
enum class TpSlMetric { UnderlyingPrice, ReturnFromEntry, MonetaryPnl };

// Construye la expresión de `metric` (§4.3): `UnderlyingPrice` -> `Current(observable)`;
// `ReturnFromEntry` -> `Current(observable)/entry_price - 1`; `MonetaryPnl` ->
// `quantity * (Current(observable) - entry_price)`. `take_profit_level`/`stop_loss_level` en
// `TpSlSpec` deben expresarse en las mismas unidades que esta métrica (precio absoluto,
// fracción o moneda, respectivamente).
ScalarExprPtr tp_sl_metric(TpSlMetric metric, ObservableId observable, double entry_price, double quantity);

// Especificación de un grupo `FirstOf` take-profit/stop-loss (§4.3). `take_profit_id`/
// `stop_loss_id` son distintos (decisión confirmada, ver ADR-P0-08): cada regla es su propio
// `Trigger`, con una guarda `Not(EventOccurred(la_otra))` en su condición para lograr "el
// primer hit gana" sin ampliar `TriggerSpec`/`EventState`.
struct TpSlSpec {
    ObservableId observable;
    TpSlMetric metric;
    double entry_price;
    double quantity;
    double take_profit_level;
    double stop_loss_level;
    std::vector<TimePoint> monitoring_times;
    Currency settlement_currency;
    EventId take_profit_id;
    EventId stop_loss_id;
};

// Grupo `POSITION_EXIT` (§4.3): `TAKE_PROFIT` (priority=10) se comprueba antes que
// `STOP_LOSS` (priority=20) en cada fecha de monitorización (ADR-P0-03); el primero que
// dispara liquida `AtHit` con `quantity * (EventValue(su_id, observable) - entry_price)` --
// paga con el fixing observado, no al nivel exacto del stop, si hay gap (§4.3). Si ninguno
// ocurre, el grupo entero vale `Zero`.
ContractPtr first_of_take_profit_stop_loss(const TpSlSpec& spec);

} // namespace templates
} // namespace payoff
} // namespace engine
