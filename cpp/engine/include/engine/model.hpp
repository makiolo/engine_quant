#pragma once

#include <string>

#include "engine/params.hpp"

namespace engine {

// Interfaz base de todo modelo registrable (PLAN.md §5.4). El registry construye
// implementaciones concretas exclusivamente a partir de un Params (ver Registry<T>::create).
class IModel {
public:
    virtual ~IModel() = default;
    virtual std::string type_name() const = 0;
};

// Hull-White de 1 factor con nivel de reversión de largo plazo constante (mismo modelo que
// `rust/crates/engine-core/src/models/hull_white.rs`, PLAN.md §7.5).
// Params requeridos: "a", "b", "sigma", "r0" (todos double).
class HullWhite1FModel : public IModel {
public:
    explicit HullWhite1FModel(const Params& params);

    std::string type_name() const override { return "HullWhite1F"; }

    double a() const;
    double b() const;
    double sigma() const;
    double r0() const;

private:
    double a_;
    double b_;
    double sigma_;
    double r0_;
};

} // namespace engine
