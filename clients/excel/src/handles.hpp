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

#include <memory>
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
#include "engine/greeks.hpp"
#include "engine/market.hpp"
#include "engine/portfolio.hpp"
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

    // Autoría/validación sin registry (PLAN_PRODUCTS.md Fase 10, §7.1), misma superficie que
    // nanobind (engine.validate_payoff_spec/Product.explain) y la C ABI
    // (engine_abi_validate_payoff_spec/engine_abi_explain_product). validate_payoff_spec no
    // necesita ningún handle -- opera directamente sobre el JSON; explain_product sí, sobre un
    // product_handle ya creado por create_product.
    std::string explain_product(const std::string& product_handle) const;
    std::vector<std::string> validate_payoff_spec(const std::string& spec_json) const;

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

    // Barrido automatico de Greeks (PLAN_GREEKS.md §8.5/§9.2, Fase 9): enumera los RiskFactor
    // candidatos de model/market para metric_name y calcula todos los que apliquen -- ver
    // engine::greeks::compute_all_greeks. metric_params_arg es un rango clave/valor como
    // params_arg en el resto de create_* (vacio/omitido = sin parametros propios de la
    // metrica interior).
    engine::greeks::GreeksReport all_greeks(
        const std::string& product_handle,
        const std::string& metric_name,
        const XLOPER12& metric_params_arg,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle,
        bool include_curve_buckets,
        bool include_second_order
    ) const;

    // Hessiano local de un trade (PLAN_BACKWARD.md §8.2/§9 Fase 1-3), mismos handles de entrada
    // que all_greeks -- ver engine::greeks::compute_hessian. factors_arg es un rango vertical de
    // strings namespaced ("model.spot", ...) igual que read_string_list ya usa para
    // measure_names; en blanco/omitido = enumeracion automatica (factors={}).
    engine::greeks::HessianReport hessian(
        const std::string& product_handle,
        const std::string& metric_name,
        const XLOPER12& metric_params_arg,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle,
        const XLOPER12& factors_arg
    ) const;

    // Producto Hessiano-vector H*v (PLAN_BACKWARD.md §8.2/§9 Fase 3), mismos handles de entrada
    // que all_greeks -- ver engine::greeks::compute_hvp. direction_arg es un rango de 2 columnas
    // [RiskFactor, Peso] (col 0 = string namespaced, col 1 = numero), obligatorio y no vacio
    // (a diferencia de factors_arg en hessian, que sí acepta la enumeracion automatica).
    engine::greeks::HvpReport hvp(
        const std::string& product_handle,
        const std::string& metric_name,
        const XLOPER12& metric_params_arg,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle,
        const XLOPER12& direction_arg
    ) const;

    // market_handle: handle devuelto por create_market (ya no un rango inline -- un
    // MarketSnapshot no polimórfico se memoiza igual que Model/Product, PLAN.md §7.15).
    // initial_guess_arg: mismo formato clave/valor que params_arg en el resto de create_*.
    engine::CalibrationResult calibrate(
        const std::string& calibrator_handle, const std::string& market_handle, const XLOPER12& initial_guess_arg
    ) const;

    // --- Portfolio (PLAN_BACKWARD.md §6.4/§9 Fase 6) ----------------------------------------
    // Tension de diseño resuelta como Opcion B (documentada en handles.cpp, junto a la
    // implementacion): un `ENGINE.PORTFOLIO.CREATE` FUNCIONAL que construye el Portfolio
    // COMPLETO de una vez a partir de un rango de handles de trade ya creados -- memoizado por
    // esa lista EXACTA (mismo patron de clave canonica que create_model/create_product/
    // create_market), en vez de una UDF `.ADD` MUTANTE sobre un handle ya existente. Una `.ADD`
    // mutante rompería el modelo de memoizacion determinista de HandleRegistry (mismos
    // parametros -> mismo handle, sin gestion de ciclo de vida por celda -- ver el comentario de
    // cabecera de este fichero, lineas 1-20): cada recalculo de Excel de la celda que la contiene
    // volveria a anadir el trade al MISMO portfolio, salvo que `.ADD` fuera idempotente
    // comprobando membresia -- complejidad no prevista por PLAN_BACKWARD.md §6.4.
    std::string create_portfolio(const std::vector<std::string>& trade_handles);

    // Mismos handles/parametros que price/hessian/hvp de trade unico, sustituyendo
    // product_handle por portfolio_handle -- misma forma de resultado (Portfolio::price/hessian/
    // hvp son envoltorios que suman/delegan sobre compute_hessian/compute_hvp/price_many YA
    // EXISTENTES, ver engine/portfolio.hpp).
    engine::PriceBatchResult portfolio_price(
        const std::string& portfolio_handle,
        const std::vector<std::string>& measure_names,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle
    ) const;

    engine::greeks::HessianReport portfolio_hessian(
        const std::string& portfolio_handle,
        const std::string& metric_name,
        const XLOPER12& metric_params_arg,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle,
        const XLOPER12& factors_arg
    ) const;

    engine::greeks::HvpReport portfolio_hvp(
        const std::string& portfolio_handle,
        const std::string& metric_name,
        const XLOPER12& metric_params_arg,
        const std::string& model_handle,
        const std::string& market_handle,
        const std::string& pricing_handle,
        const std::string& execution_handle,
        const XLOPER12& direction_arg
    ) const;

    // Libera todas las instancias memoizadas (xlAutoClose, engine_excel.cpp).
    void clear();

private:
    engine::Registries registries_;
    std::unordered_map<std::string, std::unique_ptr<engine::IModel>> models_;
    // shared_ptr (no unique_ptr, PLAN_BACKWARD.md §9 Fase 6): un mismo product_handle ya creado
    // se comparte con uno o mas Portfolio (portfolios_ de abajo) sin perder la entrada de este
    // mapa -- mismo cambio de ownership, y mismo motivo, que EngineProduct::ptr en la C ABI
    // (cpp/engine/src/abi.cpp). Todos los usos existentes de este mapa (resolve_products,
    // explain_product, price/price_batch/price_many/price_grid) solo hacen ->/.get()/dereferencia,
    // identicos para unique_ptr/shared_ptr.
    std::unordered_map<std::string, std::shared_ptr<engine::IProduct>> products_;
    std::unordered_map<std::string, engine::MarketSnapshot> markets_;
    std::unordered_map<std::string, engine::PricingContext> pricing_contexts_;
    std::unordered_map<std::string, engine::ExecutionContext> execution_contexts_;
    std::unordered_map<std::string, std::unique_ptr<engine::ICalibrator>> calibrators_;
    std::unordered_map<std::string, engine::Portfolio> portfolios_;
};

// Instancia única de proceso (una por XLL cargado en Excel), usada desde engine_excel.cpp.
HandleRegistry& shared();

} // namespace xlbridge
