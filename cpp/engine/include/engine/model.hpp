#pragma once

#include <optional>
#include <string>

#include "engine/params.hpp"
#include "engine/payoff/model_capabilities.hpp"

namespace engine {

// Interfaz base de todo modelo registrable (PLAN.md §5.4). El registry construye
// implementaciones concretas exclusivamente a partir de un Params (ver Registry<T>::create).
class IModel {
public:
    virtual ~IModel() = default;
    virtual std::string type_name() const = 0;

    // Evolucion aditiva (PLAN_PRODUCTS.md §6, §9.1: mismo patron que
    // IProduct::payoff_program()): que observables genera este modelo y bajo que medidas, para
    // que el preflight de un pricer de payoff (DependencyVisitor vs ModelCapabilities) pueda
    // rechazar una combinacion imposible ANTES de simular. `std::nullopt` (el default) significa
    // "este modelo no declara capacidades de payoff" -- los modelos legacy (HullWhite1F/2F) no
    // necesitan implementarlo para seguir funcionando con sus medidas actuales.
    virtual std::optional<payoff::ModelCapabilities> capabilities() const { return std::nullopt; }
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

// Hull-White de 2 factores (G2++, PLAN.md §7.16), segundo modelo del motor: dos factores
// latentes correlacionados en vez del único tipo corto de HullWhite1FModel (ver
// `rust/crates/engine-core/src/models/hull_white_2f.rs` para la dinámica completa).
// Params requeridos: "a", "b" (velocidades de reversión de cada factor), "sigma", "eta"
// (volatilidades), "rho" (correlación instantánea) y "r0" (desplazamiento constante `phi0`,
// coincide con el tipo corto inicial ya que los dos factores arrancan en cero).
class HullWhite2FModel : public IModel {
public:
    explicit HullWhite2FModel(const Params& params);

    std::string type_name() const override { return "HullWhite2F"; }

    double a() const;
    double b() const;
    double sigma() const;
    double eta() const;
    double rho() const;
    double r0() const;

private:
    double a_;
    double b_;
    double sigma_;
    double eta_;
    double rho_;
    double r0_;
};

// Movimiento geometrico browniano bajo Q, primer modelo equity del motor (PLAN_PRODUCTS.md §12
// Fase 5; dinamica y formula cerrada en `rust/crates/engine-core/src/models/gbm.rs`). A
// diferencia de HullWhite1F/2F -- que descuentan una curva de tipos y no declaran
// `capabilities()` -- este modelo genera UN UNICO observable de spot (`observable()`, p.ej.
// "EQ.SPOT.AAPL") y solo bajo la medida `RiskNeutralQ`: no existe ningun parametro de drift
// fisico que reconfigurar (§6, criterio de aceptacion de Fase 5 "cambiar el drift fisico no
// afecta un precio Q" -- aqui no hay tal parametro que cambiar).
// Params requeridos: "s0" (spot inicial), "r" (tipo libre de riesgo), "q" (dividend yield),
// "sigma" (volatilidad), "observable" (string, ObservableId que este modelo genera).
class GbmModel : public IModel {
public:
    explicit GbmModel(const Params& params);

    std::string type_name() const override { return "GBM"; }
    std::optional<payoff::ModelCapabilities> capabilities() const override;

    double s0() const;
    double r() const;
    double q() const;
    double sigma() const;
    const payoff::ObservableId& observable() const;

private:
    double s0_;
    double r_;
    double q_;
    double sigma_;
    payoff::ObservableId observable_;
};

} // namespace engine
