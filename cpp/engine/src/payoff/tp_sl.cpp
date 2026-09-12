#include "engine/payoff/tp_sl.hpp"

#include <stdexcept>
#include <utility>

#include "engine/payoff/predicate.hpp"

namespace engine {
namespace payoff {
namespace templates {

ScalarExprPtr tp_sl_metric(TpSlMetric metric, ObservableId observable, double entry_price, double quantity) {
    ScalarExprPtr spot = current(std::move(observable));
    switch (metric) {
        case TpSlMetric::UnderlyingPrice:
            return spot;
        case TpSlMetric::ReturnFromEntry:
            return sub(div(spot, constant(entry_price)), constant(1.0));
        case TpSlMetric::MonetaryPnl:
            return mul(constant(quantity), sub(spot, constant(entry_price)));
    }
    throw std::invalid_argument("TpSlMetric desconocida");
}

namespace {

ContractPtr settlement_cashflow(const TpSlSpec& spec, const EventId& event_id) {
    ScalarExprPtr amount = mul(
        constant(spec.quantity), sub(event_value(event_id, spec.observable), constant(spec.entry_price))
    );
    return cashflow(spec.settlement_currency, amount);
}

} // namespace

ContractPtr first_of_take_profit_stop_loss(const TpSlSpec& spec) {
    ScalarExprPtr metric = tp_sl_metric(spec.metric, spec.observable, spec.entry_price, spec.quantity);

    PredicatePtr take_profit_condition = all_of({
        greater_equal(metric, constant(spec.take_profit_level)),
        negate(event_occurred(spec.stop_loss_id)),
    });
    PredicatePtr stop_loss_condition = all_of({
        less_equal(metric, constant(spec.stop_loss_level)),
        negate(event_occurred(spec.take_profit_id)),
    });

    TriggerSpec take_profit_spec{
        spec.take_profit_id, spec.monitoring_times, take_profit_condition, Monitoring::Discrete, Settlement::AtHit,
        10, true
    };
    TriggerSpec stop_loss_spec{
        spec.stop_loss_id, spec.monitoring_times, stop_loss_condition, Monitoring::Discrete, Settlement::AtHit, 20,
        true
    };

    ContractPtr take_profit_trigger =
        trigger(std::move(take_profit_spec), settlement_cashflow(spec, spec.take_profit_id), zero());
    ContractPtr stop_loss_trigger =
        trigger(std::move(stop_loss_spec), settlement_cashflow(spec, spec.stop_loss_id), zero());

    return both({take_profit_trigger, stop_loss_trigger});
}

} // namespace templates
} // namespace payoff
} // namespace engine
