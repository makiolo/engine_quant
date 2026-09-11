#pragma once

// Registry de modelos/productos/medidas expuesto a Excel como "handles" (PLAN.md Fase 4,
// §7.8): una celda de Excel solo puede contener números/cadenas/booleanos/arrays, nunca un
// puntero a IModel/IProduct/IMeasure directamente (a diferencia de `engine.Model`/
// `engine.Product`/`engine.Measure` en Python, que sí son objetos Python opacos, PLAN.md
// §7.7). En vez de un mecanismo de "handle" ligado al ciclo de vida de la celda que lo creó
// (invalidación en recálculo, xlfGetCaller, etc. — complejidad importante y no verificable
// sin Excel instalado en este entorno), las instancias se memoizan por una clave canónica
// (nombre + parámetros ordenados, ver xlbridge::table_to_params): los mismos parámetros
// producen siempre el mismo handle, así que la fórmula es determinista y no hace falta
// liberar handles individualmente — la limitación conocida es que las instancias viven hasta
// xlAutoClose (documentado en clients/excel/README.md), no por-celda.
//
// Reutiliza exactamente el mismo `engine::Registries`/`register_builtins`/`Registry<T>::
// create`/`IMeasure::evaluate` que ya consumen cpp/engine/tests (Fase 2) y clients/python
// (Fase 3): añadir un modelo/producto/medida nuevo en bootstrap.cpp lo deja disponible aquí
// sin tocar este fichero (PLAN.md §5.4, §4).

#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "XLCALL.H"
#include "engine/bootstrap.hpp"
#include "engine/price.hpp"
#include "engine/execution_context.hpp"
#include "engine/market.hpp"
#include "engine/pricing_context.hpp"

namespace xlbridge {

class HandleRegistry {
public:
    HandleRegistry();

    std::vector<std::string> list_models() const;
    std::vector<std::string> list_products() const;
    // Nombres de ENGINE.PRICE (PLAN.md §7.15: "PV"/"DV01"/"ExpectedExposure"/"PFE95"/
    // "UnilateralCVA"), no los nombres registrados en Registry<IMeasure> -- ver
    // engine::price_measure_names().
    std::vector<std::string> list_measures() const;
    std::vector<std::string> list_calibrators() const;

    std::string create_model(const std::string& name, const XLOPER12& params_arg);
    std::string create_product(const std::string& name, const XLOPER12& params_arg);
    // Market/PricingContext/ExecutionContext (PLAN.md §7.15) no llevan nombre de tipo -- una
    // sola forma concreta cada uno, a diferencia de create_model/create_product -- así que se
    // memoizan solo por sus parámetros (mismo patrón de clave canónica que table_to_params ya
    // usaba para model/product).
    std::string create_market(const XLOPER12& params_arg);
    std::string create_context(const XLOPER12& params_arg);
    std::string create_execution(const XLOPER12& params_arg);
    // Sin parámetros que memoizar (a diferencia de create_model/create_product): un
    // ICalibrator no tiene estado propio, ver engine::HullWhite1FCalibrator -- el handle se
    // memoiza solo por `name`.
    std::string create_calibrator(const std::string& name);

    // Sustituye por completo create_measure/evaluate (PLAN.md §7.15): calcula un lote de
    // medidas nombradas de una vez sobre el mismo product/model/market/pricing/execution.
    engine::PriceResult price(
        const std::string& product_handle,
        const std::vector<std::string>& measure_names,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle
    ) const;

    // Nivel 3, lote homogeneo (PLAN.md §7.17/§7.19): product_handles debe ser una columna de
    // handles del mismo tipo/calendario, sin use_par_rate -- ver engine::price_batch.
    engine::PriceBatchResult price_batch(
        const std::vector<std::string>& product_handles,
        const std::vector<std::string>& measure_names,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle
    ) const;

    // Nivel 2, lista heterogenea (PLAN.md §7.17/§7.19): misma forma que price_batch, pero
    // product_handles puede mezclar tipos/calendarios distintos -- ver engine::price_many.
    engine::PriceBatchResult price_many(
        const std::vector<std::string>& product_handles,
        const std::vector<std::string>& measure_names,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle
    ) const;

    // Explosion de combinaciones Trades x Models x Markets (PLAN.md §7.19): pricing/execution
    // son compartidos, no forman parte de la rejilla -- ver engine::price_grid.
    engine::PriceGridResult price_grid(
        const std::vector<std::string>& product_handles,
        const std::vector<std::string>& measure_names,
        const std::vector<std::string>& model_handles,
        const std::vector<std::string>& market_handles,
        const std::string& pricing_handle,
        const std::string& execution_handle
    ) const;

    // market_handle: handle devuelto por create_market (ya no un rango inline -- un
    // MarketSnapshot no polimórfico se memoiza igual que Model/Product, PLAN.md §7.15).
    // initial_guess_arg: mismo formato clave/valor que params_arg en el resto de create_*.
    engine::CalibrationResult calibrate(
        const std::string& calibrator_handle, const std::string& market_handle, const XLOPER12& initial_guess_arg
    ) const;

    // Libera todas las instancias memoizadas (xlAutoClose, engine_excel.cpp).
    void clear();

private:
    engine::Registries registries_;
    std::unordered_map<std::string, std::unique_ptr<engine::IModel>> models_;
    std::unordered_map<std::string, std::unique_ptr<engine::IProduct>> products_;
    std::unordered_map<std::string, engine::MarketSnapshot> markets_;
    std::unordered_map<std::string, engine::PricingContext> pricing_contexts_;
    std::unordered_map<std::string, engine::ExecutionContext> execution_contexts_;
    std::unordered_map<std::string, std::unique_ptr<engine::ICalibrator>> calibrators_;
};

// Instancia única de proceso (una por XLL cargado en Excel), usada desde engine_excel.cpp.
HandleRegistry& shared();

} // namespace xlbridge
