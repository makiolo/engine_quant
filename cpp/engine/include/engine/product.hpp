#pragma once

#include <string>
#include <vector>

#include "engine/params.hpp"

namespace engine {

// Interfaz base de todo producto registrable (PLAN.md §5.4).
class IProduct {
public:
    virtual ~IProduct() = default;
    virtual std::string type_name() const = 0;
};

// IRS vanilla (mismo caso base que `rust/crates/engine-core/src/products/irs.rs`, PLAN.md
// §5.2). Params: "notional" (double, requerido), "start" (double, default 0.0),
// "payment_times" (vector<double>, requerido), "accruals" (vector<double>, requerido, mismo
// largo que payment_times), "fixed_rate" (double, opcional): si está ausente, el swap es "a
// la par" (use_par_rate() == true) y el tipo fijo se calcula en Rust, que necesita r0 del
// modelo — no disponible en esta capa.
class IrSwapProduct : public IProduct {
public:
    explicit IrSwapProduct(const Params& params);

    std::string type_name() const override { return "IRSwap"; }

    double notional() const;
    double fixed_rate() const;
    bool use_par_rate() const;
    double start() const;
    const std::vector<double>& payment_times() const;
    const std::vector<double>& accruals() const;

private:
    double notional_;
    double fixed_rate_;
    bool use_par_rate_;
    double start_;
    std::vector<double> payment_times_;
    std::vector<double> accruals_;
};

} // namespace engine
