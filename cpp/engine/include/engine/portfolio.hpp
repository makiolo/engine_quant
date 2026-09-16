#pragma once

// `engine::Portfolio` (PLAN_BACKWARD.md §6.4/§9 Fase 6): objeto first-class de orquestacion,
// conceptualmente una lista de trades (`IProduct`) valorados/arriesgados juntos bajo un UNICO
// `IModel`/`MarketSnapshot` compartido -- alcance explicitamente minimo (sin netting, colateral,
// multi-moneda, multi-modelo, ver PLAN_BACKWARD.md §6.4/§6.5/§12). No confundir con el
// `Portfolio(children)` del AST de ARCHITECTURE_REVIEW.md (composicion de CASHFLOWS dentro de un
// contrato): este `Portfolio` agrega RESULTADOS de trades ya valorados por separado, no fusiona
// sus cashflows.
//
// `price()` es un envoltorio fino sobre `engine::price_many` YA EXISTENTE (price.hpp) -- no
// cambia su comportamiento, solo le da identidad (crear una vez, invocar varias veces).
// `hessian()`/`hvp()` son la pieza nueva: suman, trade a trade, los `HessianReport`/`HvpReport`
// de `engine::greeks::compute_hessian`/`compute_hvp` (greeks.hpp) -- matematicamente exacto
// porque V(theta) = Sum_k V_k(theta) bajo el mismo modelo/mercado compartido implica
// d2V/didj = Sum_k d2V_k/didj (suma de floats ya calculados, no una nueva fuente de error
// numerico mas alla de redondeo de punto flotante).
//
// Criterio "nunca sumar incorrectamente en silencio" (PLAN_BACKWARD.md §11): un par
// (factor_i,factor_j) -- o, para `hvp()`, un factor -- que no aparezca en el HessianReport/
// HvpReport de TODOS los trades (por estar en su `skipped`, o porque su modelo/producto no
// aplica) NUNCA se suma como si faltase valiera 0.0: se omite de `entries`/`components` y se
// documenta en `Portfolio::hessian`/`hvp`'s propio `skipped`, nombrando el par/factor y el
// indice de trade (dentro del Portfolio) donde falto.
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "engine/execution_context.hpp"
#include "engine/greeks.hpp"
#include "engine/market.hpp"
#include "engine/model.hpp"
#include "engine/params.hpp"
#include "engine/price.hpp"
#include "engine/pricing_context.hpp"
#include "engine/product.hpp"

namespace engine {

class Portfolio {
public:
    Portfolio() = default;

    // `shared_ptr<const IProduct>` (no `unique_ptr`): un mismo trade puede seguir siendo usado
    // para pricing individual (p.ej. un handle ya creado) a la vez que vive dentro de uno o mas
    // `Portfolio` -- ver el cambio analogo de `EngineProduct::ptr` en la C ABI (abi.cpp).
    void add(std::shared_ptr<const IProduct> trade);
    std::size_t size() const;
    const std::vector<std::shared_ptr<const IProduct>>& trades() const;

    // Envoltorio fino sobre `engine::price_many` YA EXISTENTE (price.hpp): construye
    // `vector<const IProduct*>` a partir de `trades_` y delega, sin logica propia adicional.
    PriceBatchResult price(
        const Registries& registries, const std::vector<std::string>& measures, const IModel& model,
        const MarketSnapshot& market, const PricingContext& pricing, const ExecutionContext& execution
    ) const;

    // Suma, trade a trade, los `HessianReport` de `engine::greeks::compute_hessian` -- ver el
    // criterio de interseccion/skip en el doc-comment de arriba y en portfolio.cpp.
    // `std_error` de una entrada agregada: presente (suma en cuadratura, asumiendo independencia
    // entre las simulaciones Monte Carlo de trades distintos) solo si TODOS los trades que
    // aportan esa entrada tienen `std_error` (Monte Carlo/likelihood ratio); ausente si CUALQUIERA
    // no lo tiene (formula cerrada, Hull-White) -- mezclar MC con formula cerrada en la misma
    // entrada no tiene una propagacion de error limpia.
    greeks::HessianReport hessian(
        const Registries& registries, const std::string& metric_name, const Params& metric_params,
        const IModel& model, const MarketSnapshot& market, const PricingContext& pricing,
        const ExecutionContext& execution, const std::vector<greeks::RiskFactor>& factors = {}
    ) const;

    // Suma, trade a trade, los `HvpReport` de `engine::greeks::compute_hvp` -- mismo criterio de
    // interseccion/skip que `hessian()`, pero sobre componentes por factor (no pares); sin
    // `std_error` porque `HvpComponent` no lo lleva.
    greeks::HvpReport hvp(
        const Registries& registries, const std::string& metric_name, const Params& metric_params,
        const IModel& model, const MarketSnapshot& market, const PricingContext& pricing,
        const ExecutionContext& execution, const std::vector<greeks::RiskFactor>& factors,
        const std::vector<double>& direction
    ) const;

private:
    std::vector<std::shared_ptr<const IProduct>> trades_;
};

} // namespace engine
