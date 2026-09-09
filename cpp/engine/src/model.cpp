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

} // namespace engine
