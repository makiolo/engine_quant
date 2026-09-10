#include "engine/pricing_context.hpp"

#include <stdexcept>

namespace engine {

PricingContext::PricingContext(const Params& params)
    : pricing_date_(get_double(params, "pricing_date", 0.0)),
      n_paths_(static_cast<std::uint64_t>(get_double(params, "n_paths"))),
      n_steps_(static_cast<std::size_t>(get_double(params, "n_steps"))),
      seed_(static_cast<std::uint64_t>(get_double(params, "seed"))) {
    if (n_paths_ == 0) {
        throw std::invalid_argument("PricingContext: n_paths debe ser mayor que 0");
    }
    if (n_steps_ == 0) {
        throw std::invalid_argument("PricingContext: n_steps debe ser mayor que 0");
    }
}

} // namespace engine
