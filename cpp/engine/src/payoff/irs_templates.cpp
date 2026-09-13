#include "engine/payoff/irs_templates.hpp"

#include <stdexcept>
#include <utility>

#include "engine/payoff/expression.hpp"

namespace engine {
namespace payoff {
namespace templates {

ContractPtr irs_swap(const IrsSwapSpec& spec) {
    if (spec.payment_times.empty()) {
        throw std::invalid_argument("irs_swap: payment_times no puede estar vacío");
    }
    if (spec.payment_times.size() != spec.accruals.size()) {
        throw std::invalid_argument("irs_swap: payment_times y accruals deben tener igual longitud");
    }

    ContractPtr floating_leg = both({
        when(spec.start, cashflow(spec.currency, constant(spec.notional))),
        when(spec.payment_times.back(), cashflow(spec.currency, constant(-spec.notional))),
    });

    std::vector<ContractPtr> fixed_flows;
    fixed_flows.reserve(spec.payment_times.size());
    for (std::size_t i = 0; i < spec.payment_times.size(); ++i) {
        double amount = spec.notional * spec.fixed_rate * spec.accruals[i];
        fixed_flows.push_back(when(spec.payment_times[i], cashflow(spec.currency, constant(amount))));
    }
    ContractPtr fixed_leg = both(std::move(fixed_flows));

    return both({floating_leg, give(fixed_leg)});
}

} // namespace templates
} // namespace payoff
} // namespace engine
