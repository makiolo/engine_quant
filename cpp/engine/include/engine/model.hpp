#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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

    // Extensión aditiva (PLAN_GREEKS.md §4.2): serializa los `Params` con los que se
    // reconstruiría un modelo IDÉNTICO vía `Registry<IModel>::create(type_name(), to_params())`.
    // Todo modelo YA se construye desde un `Params` -- `to_params()` es la operación inversa, y
    // con ella "bumpear un parámetro de modelo" se reduce a "leer el double de esa clave,
    // sumarle h, reconstruir" (`engine::greeks::compute_greek`), sin un método virtual nuevo por
    // parámetro ni por modelo. Pura virtual: los cuatro modelos concretos de este archivo son los
    // únicos que implementan `IModel` hoy, así que no hay ningún consumidor externo que rompa.
    virtual Params to_params() const = 0;
};

// Hull-White de 1 factor con nivel de reversión de largo plazo constante (mismo modelo que
// `rust/crates/engine-core/src/models/hull_white.rs`, PLAN.md §7.5).
// Params requeridos: "a", "b", "sigma", "r0" (todos double).
class HullWhite1FModel : public IModel {
public:
    explicit HullWhite1FModel(const Params& params);

    std::string type_name() const override { return "HullWhite1F"; }
    Params to_params() const override;

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
    Params to_params() const override;

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
    Params to_params() const override;

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

// Movimiento geometrico browniano bajo P, primer modelo fisico del motor (PLAN_PRODUCTS.md §12
// Fase 7; dinamica en `rust/crates/engine-core/src/models/gbm_p.rs::GbmP`). Estructuralmente
// paralelo a GbmModel pero NO reconfigurable a partir de el: `mu` es un drift fisico total (no
// libre de riesgo, no ajustado por dividendo), y `capabilities()` solo declara `PhysicalP` -- el
// preflight de las medidas Fase 7 (forecast/hit-probability/P&L bajo P) rechaza este modelo si se
// le pide una medida Q, igual que GbmModel se rechaza si se le pide una medida P (§6: "el motor
// debe impedir que la diferencia quede escondida en un nombre ambiguo de modelo").
// Params requeridos: "s0" (spot inicial), "mu" (drift fisico total), "sigma" (volatilidad),
// "observable" (string, ObservableId que este modelo genera).
class GbmPModel : public IModel {
public:
    explicit GbmPModel(const Params& params);

    std::string type_name() const override { return "GBM_P"; }
    std::optional<payoff::ModelCapabilities> capabilities() const override;
    Params to_params() const override;

    double s0() const;
    double mu() const;
    double sigma() const;
    const payoff::ObservableId& observable() const;

private:
    double s0_;
    double mu_;
    double sigma_;
    payoff::ObservableId observable_;
};

// Movimiento geometrico browniano MULTI-ACTIVO correlacionado bajo Q (PLAN_IMPROVE_NOTEBOOK.md
// Fase 3, §2 "Modelo multi-activo correlacionado"): generaliza GbmModel (un unico observable) a
// N observables que comparten el mismo browniano bajo una matriz de correlacion instantanea
// constante -- primer modelo del motor para baskets/spreads/worst-of/best-of/quanto (Fase 3,
// friccion 3 de PLAN_IMPROVE_NOTEBOOK.md §1). Clase SEPARADA de GbmModel (no una generalizacion
// in-place): GbmModel::observable()/s0()/r()/q()/sigma() son escalares y ya los consume
// risk_neutral_price_gbm/greeks/hedge/etc. para N=1 -- cambiar esa forma rompe ese contrato.
// `PayoffPriceQMeasure` (measure.cpp) dispatch-ea a esta clase O a GbmModel segun cual acepte el
// dynamic_cast (Fase 3 §2 punto 3 de PLAN_IMPROVE_NOTEBOOK.md: misma medida "PayoffPriceQ" para
// ambos, no una medida separada).
//
// Params requeridos (PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2 punto 4, decision de diseno tomada:
// Opcion A -- un unico string delimitado, ParamValue NO gana un variante vector<string>):
// - "observables": string con los ObservableId separados por comas, p.ej.
//   "EQ.SPOT.A,EQ.SPOT.B,EQ.SPOT.C" (uno por activo, define n_assets = numero de elementos).
// - "s0"/"r"/"q"/"sigma": vector<double>, uno por activo, MISMO ORDEN que "observables".
// - "correlation": vector<double> aplanado FILA A FILA, tamano n_assets*n_assets
//   (correlation[i*n_assets+j] es la correlacion entre el activo i y el activo j).
class GbmBasketModel : public IModel {
public:
    explicit GbmBasketModel(const Params& params);

    std::string type_name() const override { return "GbmBasket"; }
    std::optional<payoff::ModelCapabilities> capabilities() const override;
    Params to_params() const override;

    std::size_t n_assets() const { return observables_.size(); }
    const std::vector<payoff::ObservableId>& observables() const { return observables_; }
    const std::vector<double>& s0() const { return s0_; }
    const std::vector<double>& r() const { return r_; }
    const std::vector<double>& q() const { return q_; }
    const std::vector<double>& sigma() const { return sigma_; }
    // Aplanada fila a fila, n_assets*n_assets -- ver el doc-comment de la clase.
    const std::vector<double>& correlation() const { return correlation_; }

private:
    std::vector<payoff::ObservableId> observables_;
    std::vector<double> s0_;
    std::vector<double> r_;
    std::vector<double> q_;
    std::vector<double> sigma_;
    std::vector<double> correlation_;
};

} // namespace engine
