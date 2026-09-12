#include "engine/payoff/barrier_templates.hpp"

#include <utility>

#include "engine/payoff/expression.hpp"
#include "engine/payoff/predicate.hpp"

namespace engine {
namespace payoff {
namespace templates {

namespace {

ContractPtr barrier_trigger(
    EventId id, PredicatePtr condition, std::vector<TimePoint> monitoring_times, ContractPtr on_hit,
    ContractPtr on_miss
) {
    TriggerSpec spec{
        std::move(id), std::move(monitoring_times), std::move(condition), Monitoring::Discrete, Settlement::AtHit,
        0, true
    };
    return trigger(std::move(spec), std::move(on_hit), std::move(on_miss));
}

} // namespace

ContractPtr up_and_in(
    EventId id, ObservableId observable, double barrier, std::vector<TimePoint> monitoring_times,
    ContractPtr underlying_contract
) {
    PredicatePtr condition = greater_equal(current(std::move(observable)), constant(barrier));
    return barrier_trigger(std::move(id), condition, std::move(monitoring_times), std::move(underlying_contract), zero());
}

ContractPtr down_and_out(
    EventId id, ObservableId observable, double barrier, std::vector<TimePoint> monitoring_times,
    ContractPtr underlying_contract, ContractPtr rebate
) {
    PredicatePtr condition = less_equal(current(std::move(observable)), constant(barrier));
    return barrier_trigger(
        std::move(id), condition, std::move(monitoring_times), std::move(rebate), std::move(underlying_contract)
    );
}

ContractPtr double_knock_out(
    EventId id, ObservableId observable, double barrier_low, double barrier_high,
    std::vector<TimePoint> monitoring_times, ContractPtr underlying_contract, ContractPtr rebate
) {
    ScalarExprPtr spot = current(std::move(observable));
    PredicatePtr condition =
        any_of({less_equal(spot, constant(barrier_low)), greater_equal(spot, constant(barrier_high))});
    return barrier_trigger(
        std::move(id), condition, std::move(monitoring_times), std::move(rebate), std::move(underlying_contract)
    );
}

std::vector<TimePoint> window_schedule(
    const std::vector<TimePoint>& full_schedule, TimePoint window_start, TimePoint window_end
) {
    std::vector<TimePoint> filtered;
    for (TimePoint t : full_schedule) {
        bool after_or_at_start = !time_less(t, window_start);
        bool before_or_at_end = !time_less(window_end, t);
        if (after_or_at_start && before_or_at_end) filtered.push_back(t);
    }
    return filtered;
}

} // namespace templates
} // namespace payoff
} // namespace engine
