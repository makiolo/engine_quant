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

} // namespace engine
