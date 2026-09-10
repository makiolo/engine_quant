#pragma once

#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/execution_context.hpp"
#include "engine/market.hpp"
#include "engine/measure.hpp"
#include "engine/pricing_context.hpp"

namespace engine {

// Un resultado nombrado del lote de ENGINE.CALC (PLAN.md §7.15).
struct CalcResultEntry {
    std::string measure_name;
    MeasureResult result;
};
using CalcResult = std::vector<CalcResultEntry>;

// Nombres de medida que acepta ENGINE.CALC ("PV", "DV01", "ExpectedExposure", "PFE95",
// "UnilateralCVA") -- el vocabulario de cara al usuario, no los nombres registrados en
// `Registry<IMeasure>` (que siguen siendo el mecanismo de extensión de PLAN.md §5.4: añadir
// una medida nueva a `ENGINE.CALC` es una `IMeasure` + una línea en `bootstrap.cpp` + una
// entrada en la tabla de `calc.cpp`, nada más). Sustituye a `Registry<IMeasure>::list()`,
// que antes exponía `ENGINE.LIST_MEASURES` directamente (PLAN.md §7.15).
std::vector<std::string> calc_measure_names();

// Orquesta el cálculo de un lote de medidas nombradas: agrupa los nombres que comparten el
// mismo cálculo subyacente (p.ej. "ExpectedExposure"/"PFE95" -> una sola llamada a
// ExposureProfileMeasure::evaluate, ver calc.cpp) y devuelve los resultados en el mismo
// orden en que se pidieron. Único punto que conoce el mapeo nombre-de-CALC -> medida
// registrada; Python/Excel/la C ABI lo consumen, no lo reimplementan.
//
// Lanza std::invalid_argument si algún nombre de `measure_names` no está en
// `calc_measure_names()`.
CalcResult calc(
    const Registries& registries,
    const IProduct& product,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

// --- Nivel 3: lote homogéneo (PLAN.md §7.17/§7.19) ------------------------------------------
// Un `CalcResult` por trade, mismo orden que `products` -- `trade_index` es explícito (no una
// posición implícita en una lista anidada) para que el resultado se pueda "aplanar" igual en
// las cinco capas (C++/C ABI/Python/Excel devuelven filas con su índice, nunca listas de
// listas). Requiere: todos los `products` del mismo tipo registrado (`IProduct::type_name()`)
// y, para `IrSwapProduct`, el mismo calendario (`start`/`payment_times`/`accruals`) y
// `use_par_rate() == false` en todos -- lanza `std::invalid_argument` si no se cumple. Con un
// único tipo de producto real (`IrSwapProduct`) hoy, "agrupar por tipo" es un único grupo por
// construcción; el resto de la validación (calendario) sí es necesaria ya.
struct CalcBatchResultEntry {
    std::size_t trade_index;
    CalcResult measures;
};
using CalcBatchResult = std::vector<CalcBatchResultEntry>;

CalcBatchResult calc_batch(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

// --- Nivel 2: lista heterogénea (PLAN.md §7.17/§7.19) ---------------------------------------
// Misma forma de resultado que `calc_batch`, pero `products` puede mezclar tipos/calendarios
// distintos: se agrupan internamente por `(type_name(), calendario)` y cada grupo se resuelve
// con `calc_batch` (grupos de tamaño 1 incluidos, sin caso especial), recomponiendo el
// resultado en el orden de entrada original. Nunca lanza por heterogeneidad -- esa es
// precisamente la diferencia con `calc_batch`.
CalcBatchResult calc_many(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

// --- Explosión de combinaciones: Trades × Models × Markets (PLAN.md §7.19) ------------------
// Por cada par (modelo, mercado) de `models`×`markets`, llama a `calc_many` sobre `products`
// entero -- `PricingContext`/`ExecutionContext` son compartidos por toda la rejilla (no forman
// parte de la explosión). Resultado plano: una fila por (trade, modelo, mercado), con sus tres
// índices explícitos, mismo espíritu que `CalcBatchResultEntry::trade_index`.
struct CalcGridResultEntry {
    std::size_t trade_index;
    std::size_t model_index;
    std::size_t market_index;
    CalcResult measures;
};
using CalcGridResult = std::vector<CalcGridResultEntry>;

CalcGridResult calc_grid(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const std::vector<const IModel*>& models,
    const std::vector<MarketSnapshot>& markets,
    const PricingContext& pricing,
    const ExecutionContext& execution
);

} // namespace engine
