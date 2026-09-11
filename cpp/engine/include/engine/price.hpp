#pragma once

#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/execution_context.hpp"
#include "engine/market.hpp"
#include "engine/measure.hpp"
#include "engine/params.hpp"
#include "engine/pricing_context.hpp"

namespace engine {

// Un resultado nombrado del lote de ENGINE.PRICE (PLAN.md §7.15).
struct PriceResultEntry {
    std::string measure_name;
    MeasureResult result;
};
using PriceResult = std::vector<PriceResultEntry>;

// Una medida con su configuración opcional (PLAN_REAPI.md §6 Fase 3, propuesta 3 --
// "measures tipadas"): generaliza el antiguo `measure_names: vector<string>` a
// `vector<MeasureSpec>`. `params` vacío es exactamente equivalente al nombre "pelado" de
// antes -- por eso las sobrecargas `vector<string>` de `price`/`price_batch`/`price_many`/
// `price_grid` siguen existiendo (construyen `MeasureSpec{name, {}}` internamente) y no
// rompen a ningún consumidor existente (`test_price.py`/`abi_c_smoke.c`/Excel/ejemplos).
struct MeasureSpec {
    std::string name;
    Params params;
};

// Nombres de medida "de fábrica" (PLAN_REAPI.md §6 Fase 3): el `Registry<IMeasure>` completo
// (`register_builtins`) más los dos alias heredados de PLAN.md §7.15 que no viven en el
// registry como tipo propio ("ExpectedExposure"/"PFE95", que envuelven "ExposureProfile" --
// ver `price.cpp`). Solo para *descubrir* nombres (`ENGINE.LIST_MEASURES`) -- `price()`/
// `price_batch()`/`price_many()`/`price_grid()` ya NO están limitados a esta lista: resuelven
// cualquier nombre presente en `registries.measures` directamente (decisión de
// PLAN_REAPI.md §5: se retira `calc_measure_name_mappings()` como tabla curada cerrada).
std::vector<std::string> price_measure_names(const Registries& registries);

// Orquesta el cálculo de un lote de medidas nombradas sobre un único trade: agrupa las specs
// que comparten el mismo cálculo subyacente y la misma configuración (p.ej. "ExpectedExposure"
// + "PFE95" comparten "ExposureProfile" -- una sola simulación Monte Carlo para ambas, no dos)
// y devuelve los resultados en el mismo orden en que se pidieron.
//
// Lanza std::invalid_argument si algún `MeasureSpec::name` no resuelve a una medida conocida
// (ni está en `registries.measures` ni es uno de los alias heredados).
PriceResult price(
    const Registries& registries,
    const IProduct& product,
    const std::vector<MeasureSpec>& measures,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

// Sobrecarga retrocompatible (PLAN.md §7.15, PLAN_REAPI.md §6 Fase 3): nombres "pelados",
// equivalente a pasar `MeasureSpec{name, {}}` por cada uno -- ningún consumidor que solo pase
// nombres (sin configuración por medida) necesita cambiar una línea.
PriceResult price(
    const Registries& registries,
    const IProduct& product,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

// --- Nivel 3: lote homogéneo (PLAN.md §7.17/§7.19) ------------------------------------------
// Un `PriceResult` por trade, mismo orden que `products` -- `trade_index` es explícito (no una
// posición implícita en una lista anidada) para que el resultado se pueda "aplanar" igual en
// las cinco capas (C++/C ABI/Python/Excel devuelven filas con su índice, nunca listas de
// listas). Requiere: todos los `products` del mismo tipo registrado (`IProduct::type_name()`)
// y, para `IrSwapProduct`, el mismo calendario (`start`/`payment_times`/`accruals`) y
// `use_par_rate() == false` en todos -- lanza `std::invalid_argument` si no se cumple. Con un
// único tipo de producto real (`IrSwapProduct`) hoy, "agrupar por tipo" es un único grupo por
// construcción; el resto de la validación (calendario) sí es necesaria ya.
//
// A diferencia de `price()`, el lote sigue limitado a las medidas que conoce
// `evaluate_batch_registered_measure` en `price.cpp` ("PV"/"DV01"/"UnilateralCVA"/
// "ExposureProfile", más los alias "ExpectedExposure"/"PFE95") -- con un único producto real
// no hay genericidad real que ganar todavía generalizando el despacho de lote al registry
// (mismo argumento que ya justificó no generalizar la C ABI de calibración hasta el segundo
// calibrador, PLAN.md §7.18).
struct PriceBatchResultEntry {
    std::size_t trade_index;
    PriceResult measures;
};
using PriceBatchResult = std::vector<PriceBatchResultEntry>;

PriceBatchResult price_batch(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<MeasureSpec>& measures,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

PriceBatchResult price_batch(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

// --- Nivel 2: lista heterogénea (PLAN.md §7.17/§7.19) ---------------------------------------
// Misma forma de resultado que `price_batch`, pero `products` puede mezclar tipos/calendarios
// distintos: se agrupan internamente por `(type_name(), calendario)` y cada grupo se resuelve
// con `price_batch` (grupos de tamaño 1 incluidos, sin caso especial), recomponiendo el
// resultado en el orden de entrada original. Nunca lanza por heterogeneidad -- esa es
// precisamente la diferencia con `price_batch`.
PriceBatchResult price_many(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<MeasureSpec>& measures,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

PriceBatchResult price_many(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

// --- Explosión de combinaciones: Trades × Models × Markets (PLAN.md §7.19) ------------------
// Por cada par (modelo, mercado) de `models`×`markets`, llama a `price_many` sobre `products`
// entero -- `PricingContext`/`ExecutionContext` son compartidos por toda la rejilla (no forman
// parte de la explosión). Resultado plano: una fila por (trade, modelo, mercado), con sus tres
// índices explícitos, mismo espíritu que `PriceBatchResultEntry::trade_index`.
struct PriceGridResultEntry {
    std::size_t trade_index;
    std::size_t model_index;
    std::size_t market_index;
    PriceResult measures;
};
using PriceGridResult = std::vector<PriceGridResultEntry>;

PriceGridResult price_grid(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<MeasureSpec>& measures,
    const std::vector<const IModel*>& models,
    const std::vector<MarketSnapshot>& markets,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

PriceGridResult price_grid(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const std::vector<const IModel*>& models,
    const std::vector<MarketSnapshot>& markets,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

} // namespace engine
