#include "engine/product.hpp"

#include <stdexcept>

namespace engine {

IrSwapProduct::IrSwapProduct(const Params& params)
    : notional_(get_double(params, "notional")),
      fixed_rate_(get_double(params, "fixed_rate", 0.0)),
      use_par_rate_(!contains(params, "fixed_rate")),
      start_(get_double(params, "start", 0.0)),
      payment_times_(get_vector(params, "payment_times")),
      accruals_(get_vector(params, "accruals")) {
    if (payment_times_.size() != accruals_.size()) {
        throw std::invalid_argument(
            "IrSwapProduct: payment_times y accruals deben tener el mismo largo");
    }
}

double IrSwapProduct::notional() const { return notional_; }
double IrSwapProduct::fixed_rate() const { return fixed_rate_; }
bool IrSwapProduct::use_par_rate() const { return use_par_rate_; }
double IrSwapProduct::start() const { return start_; }
const std::vector<double>& IrSwapProduct::payment_times() const { return payment_times_; }
const std::vector<double>& IrSwapProduct::accruals() const { return accruals_; }

} // namespace engine
