#pragma once

namespace engine {
namespace payoff {

// Medida probabilistica bajo la que se obtuvo un resultado (PLAN_PRODUCTS.md §6). Fase 4 solo
// produce `DeterministicScenario`: una unica ruta de mercado conocida, sin simulacion Monte
// Carlo ni calibracion fisica. `RiskNeutralQ` (Fase 5+) y `PhysicalP` (Fase 7+) llegan cuando
// existe un `IModel` que genere rutas bajo esas medidas.
//
// En su propio header (separado de `measures.hpp`) para que `model_capabilities.hpp` pueda
// declarar `ModelCapabilities::supported_measures` sin arrastrar el resto de `measures.hpp`
// (`Contract`/`DependencyVisitor`/`MarketPath`/...) -- `model.hpp` (que incluye
// `model_capabilities.hpp`) e incluir `measures.hpp` desde alli crearia un ciclo, ya que
// `measures.hpp` necesita `GbmModel` (declarado en `model.hpp`) para `risk_neutral_price_gbm`.
enum class ProbabilityMeasure { RiskNeutralQ, PhysicalP, DeterministicScenario };

} // namespace payoff
} // namespace engine
