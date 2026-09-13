#include "engine/model.hpp"

namespace engine {

HullWhite1FModel::HullWhite1FModel(const Params& params)
    : a_(get_double(params, "a")),
      b_(get_double(params, "b")),
      sigma_(get_double(params, "sigma")),
      r0_(get_double(params, "r0")) {}

double HullWhite1FModel::a() const { return a_; }
double HullWhite1FModel::b() const { return b_; }
double HullWhite1FModel::sigma() const { return sigma_; }
double HullWhite1FModel::r0() const { return r0_; }

HullWhite2FModel::HullWhite2FModel(const Params& params)
    : a_(get_double(params, "a")),
      b_(get_double(params, "b")),
      sigma_(get_double(params, "sigma")),
      eta_(get_double(params, "eta")),
      rho_(get_double(params, "rho")),
      r0_(get_double(params, "r0")) {}

double HullWhite2FModel::a() const { return a_; }
double HullWhite2FModel::b() const { return b_; }
double HullWhite2FModel::sigma() const { return sigma_; }
double HullWhite2FModel::eta() const { return eta_; }
double HullWhite2FModel::rho() const { return rho_; }
double HullWhite2FModel::r0() const { return r0_; }

GbmModel::GbmModel(const Params& params)
    : s0_(get_double(params, "s0")),
      r_(get_double(params, "r")),
      q_(get_double(params, "q")),
      sigma_(get_double(params, "sigma")),
      observable_(payoff::ObservableId{get_string(params, "observable")}) {}

std::optional<payoff::ModelCapabilities> GbmModel::capabilities() const {
    payoff::ModelCapabilities caps;
    caps.generated_observables = {observable_};
    caps.supported_measures = {payoff::ProbabilityMeasure::RiskNeutralQ};
    return caps;
}

double GbmModel::s0() const { return s0_; }
double GbmModel::r() const { return r_; }
double GbmModel::q() const { return q_; }
double GbmModel::sigma() const { return sigma_; }
const payoff::ObservableId& GbmModel::observable() const { return observable_; }

} // namespace engine
