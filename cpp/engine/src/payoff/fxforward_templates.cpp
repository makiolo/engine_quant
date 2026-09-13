#include "engine/payoff/fxforward_templates.hpp"

#include "engine/payoff/expression.hpp"

namespace engine {
namespace payoff {
namespace templates {

ContractPtr fx_forward(const FxForwardSpec& spec) {
    double sign = spec.buy_foreign ? 1.0 : -1.0;
    return when(
        spec.maturity, both({
                           cashflow(spec.foreign_currency, constant(sign * spec.notional_foreign)),
                           cashflow(spec.domestic_currency, constant(-sign * spec.notional_foreign * spec.forward_rate)),
                       })
    );
}

} // namespace templates
} // namespace payoff
} // namespace engine
