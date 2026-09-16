/* engine/abi.h -- API universal en C ABI (PLAN.md Fase 6, §5.5).
 *
 * A diferencia del resto de cpp/engine/include/engine/*.hpp (C++, consumidos directamente
 * por clients/python via nanobind y clients/excel via el bridge Excel), este header es
 * deliberadamente C puro: sin plantillas, sin std::string/std::vector/excepciones en la
 * frontera, para que cualquier lenguaje con FFI a C (Julia via ccall, .NET via P/Invoke, Go
 * via cgo, o C/C++ directamente) pueda enlazarlo sin un compilador de C++ ni conocer nada de
 * cxx/nanobind. No es una cuarta API distinta (PLAN.md §5.5): es la misma superficie que ya
 * consumen Python/Excel (Registries/register_builtins/Registry<T>::create/engine::price, mas
 * Market/PricingContext/ExecutionContext de PLAN.md §7.15) expresada en C ABI.
 *
 * Convenciones de esta ABI (validas para toda funcion de este header salvo que se diga lo
 * contrario):
 *   - Los handles (EngineModel*, EngineProduct*) son opacos y de ownership explicito: quien
 *     los crea (engine_abi_create_*) debe liberarlos (engine_abi_free_*) exactamente una vez.
 *     Nunca se debe hacer free()/delete directamente sobre ellos. Market/PricingContext/
 *     ExecutionContext NO son handles (no son polimorficos, no tienen ciclo de vida que
 *     gestionar): se pasan como structs planos por valor/puntero-const directamente a
 *     engine_abi_price, igual que EngineParam.
 *   - Las cadenas de entrada (const char*) son UTF-8, terminadas en NUL, y no se retienen mas
 *     alla de la duracion de la llamada (se copian internamente si hace falta guardarlas).
 *   - Los structs planos que devuelven datos owned por esta libreria (EngineMeasureResult,
 *     EnginePriceResultEntry, las listas de engine_abi_list_*) se liberan con su
 *     engine_abi_free_* correspondiente, nunca con el free()/delete del lenguaje que consume
 *     la ABI (los allocators pueden no coincidir entre el .dll/.so de este motor y el
 *     runtime del consumidor).
 *   - Ninguna excepcion de C++ cruza esta frontera (comportamiento indefinido en C): toda
 *     funcion que puede fallar devuelve un valor centinela (NULL, 0, != 0 segun el caso,
 *     documentado por funcion) y deja el detalle en engine_abi_last_error().
 *   - engine_abi_version() antes de asumir el layout de cualquier struct de este header: solo
 *     sube cuando cambia el layout de un struct ya publicado o la firma de una funcion ya
 *     publicada, nunca al anadir una funcion o un campo nuevo al final de un struct.
 */
#ifndef ENGINE_ABI_H
#define ENGINE_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ENGINE_ABI_BUILD)
#    define ENGINE_ABI_API __declspec(dllexport)
#  else
#    define ENGINE_ABI_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define ENGINE_ABI_API __attribute__((visibility("default")))
#else
#  define ENGINE_ABI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Version de esta ABI (PLAN.md §5.5). Sube a 2 en PLAN.md §7.15: se elimina EngineMeasure/
 * engine_abi_create_measure/engine_abi_evaluate (sustituidos por engine_abi_price), se
 * elimina engine_abi_set_compute_backend/engine_abi_get_compute_backend (el backend pasa a
 * ser un campo de EngineExecutionContext, no un estado global), y cambia el layout de
 * EngineMarketSnapshot (gana hazard_rate/recovery_rate). Sube a 3 en PLAN.md §7.18: con un
 * segundo ICalibrator (HullWhite2F) ya registrado, se elimina EngineHullWhiteCalibration/
 * engine_abi_calibrate_hull_white (especificos de HullWhite1F) en favor de un
 * EngineCalibrator opaco + engine_abi_create_calibrator/engine_abi_calibrate genericos, misma
 * forma que EngineModel/engine_abi_create_model -- la capa C++ (engine::ICalibrator) ya era
 * generica desde PLAN.md §7.14, esta ABI solo se pone al dia. Comprobar antes de asumir el
 * layout de cualquier struct de este header en un binario compilado contra una version
 * futura. */
ENGINE_ABI_API int engine_abi_version(void);

/* --- Handles opacos (Model/Product/Calibrator: polimorficos, con ciclo de vida) ---------- */

typedef struct EngineModel EngineModel;
typedef struct EngineProduct EngineProduct;
typedef struct EngineCalibrator EngineCalibrator;

/* --- Parametros (PLAN.md §5.5: "structs planos / punteros + longitud", igual bag de
 * parametros que engine::Params -- ver engine/params.hpp -- expresado sin std::variant) --- */

typedef enum EngineParamKind {
    ENGINE_PARAM_DOUBLE = 0,
    ENGINE_PARAM_VECTOR = 1,
    ENGINE_PARAM_BOOL = 2,
    ENGINE_PARAM_STRING = 3 /* PLAN_PRODUCTS.md Fase 3: unico consumidor hoy es
                             * engine_abi_create_product("Payoff", ...), clave "spec". Solo
                             * direccion de entrada (to_params); export_params() (salida de
                             * calibracion) no produce strings, ver abi.cpp. */
} EngineParamKind;

typedef struct EngineParam {
    const char* key;      /* UTF-8, NUL-terminado; no se retiene tras la llamada */
    EngineParamKind kind;
    double scalar;         /* valido si kind es ENGINE_PARAM_DOUBLE o ENGINE_PARAM_BOOL (0.0/1.0) */
    const double* values;  /* valido (no NULL) si kind es ENGINE_PARAM_VECTOR */
    size_t count;           /* longitud de values; valido si kind es ENGINE_PARAM_VECTOR */
    const char* string_value; /* anadido al FINAL de la struct (PLAN_PRODUCTS.md Fase 3): valido
                                * (no NULL) si kind es ENGINE_PARAM_STRING; UTF-8, NUL-terminado,
                                * no se retiene tras la llamada (se copia internamente). Anadir un
                                * campo al final de un struct ya publicado NO sube
                                * engine_abi_version() (ver el comentario de esa funcion, arriba
                                * en este archivo): un caller compilado contra un abi.h anterior a
                                * este campo nunca puede construir kind == ENGINE_PARAM_STRING, asi
                                * que esta libreria nunca necesita leer string_value para un
                                * EngineParam construido por ese caller. */
} EngineParam;

/* --- Listado de modelos/productos registrados (PLAN.md §5.4) y medidas de ENGINE.PRICE
 * (PLAN.md §7.15: engine_abi_list_measures devuelve los nombres de cara al usuario -- "PV",
 * "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA" -- no los nombres registrados en
 * Registry<IMeasure>, que quedan como detalle interno de extension) -----------------------
 * *out_names queda apuntando a un array de `size_t` cadenas C (o NULL si el array esta
 * vacio); liberar siempre con engine_abi_free_string_list, incluso si el count devuelto es 0
 * y *out_names es NULL (no hace falta comprobarlo, engine_abi_free_string_list acepta NULL). */
ENGINE_ABI_API size_t engine_abi_list_models(const char*** out_names);
ENGINE_ABI_API size_t engine_abi_list_products(const char*** out_names);
ENGINE_ABI_API size_t engine_abi_list_measures(const char*** out_names);
/* Calibradores registrados (PLAN.md §7.18), nombres de Registry<ICalibrator> -- hoy
 * "HullWhite1F"/"HullWhite2F", uno por modelo del motor con calibrador implementado. */
ENGINE_ABI_API size_t engine_abi_list_calibrators(const char*** out_names);
ENGINE_ABI_API void engine_abi_free_string_list(const char** names, size_t count);

/* --- Creacion / liberacion de modelos, productos y calibradores --------------------------
 * Devuelven NULL si `name` no esta registrado o los parametros no son validos para ese tipo
 * (ver engine_abi_last_error). engine_abi_create_calibrator no toma parametros -- ICalibrator
 * no tiene estado propio, ver engine::HullWhite1FCalibrator/HullWhite2FCalibrator: la
 * estimacion inicial se pasa en cada llamada a engine_abi_calibrate, no al crear el handle. */
ENGINE_ABI_API EngineModel* engine_abi_create_model(
    const char* name, const EngineParam* params, size_t n_params
);
ENGINE_ABI_API EngineProduct* engine_abi_create_product(
    const char* name, const EngineParam* params, size_t n_params
);
ENGINE_ABI_API EngineCalibrator* engine_abi_create_calibrator(const char* name);

/* Liberar exactamente una vez cada handle devuelto por un engine_abi_create_*; NULL se
 * ignora (como free()). */
ENGINE_ABI_API void engine_abi_free_model(EngineModel* model);
ENGINE_ABI_API void engine_abi_free_product(EngineProduct* product);
ENGINE_ABI_API void engine_abi_free_calibrator(EngineCalibrator* calibrator);

/* --- Autoria/validacion de PayoffProduct sin registry (PLAN_PRODUCTS.md Fase 10, SS7.1) -----
 * Superficie de autoria (validar/explicar) sin crear ningun handle, para las mismas UX que
 * nanobind/Excel (engine.validate_payoff_spec / Product.explain -- ver clients/python/src/
 * engine_py_ext.cpp, clients/excel/src/handles.cpp). */

/* Valida un documento engine.payoff/v1 (JSON, misma clave "spec" que engine_abi_create_product
 * con name="Payoff") sin construir ningun EngineProduct. Devuelve 0 si el spec es valido, != 0
 * si no -- el detalle (todos los errores agregados, mismo formato que si
 * engine_abi_create_product("Payoff", ...) fallara) se consulta con engine_abi_last_error. */
ENGINE_ABI_API int engine_abi_validate_payoff_spec(const char* spec_json);

/* Arbol/cashflows legibles de un producto ya creado (PayoffProduct) o solo su type_name
 * (productos legacy sin AST propio -- ver IProduct::explain()). Misma convencion snprintf que
 * engine_abi_last_error: escribe hasta buffer_len bytes (incluido el NUL) y devuelve la
 * longitud real del mensaje sin contar el NUL; buffer/buffer_len pueden ser NULL/0 para solo
 * consultar la longitud. `product` NULL es un error (ver engine_abi_last_error), no un explain
 * vacio. */
ENGINE_ABI_API size_t engine_abi_explain_product(const EngineProduct* product, char* buffer, size_t buffer_len);

/* --- Market / PricingContext / ExecutionContext (PLAN.md §7.15) -------------------------
 * Structs planos, no handles: se construyen y se pasan directamente a
 * engine_abi_calibrate/engine_abi_price, sin creacion/liberacion propia. */

typedef struct EngineMarketSnapshot {
    const double* pillars;    /* anios desde hoy, estrictamente creciente */
    const double* zero_rates; /* tipos cero de capitalizacion continua, mismo largo */
    size_t count;
    double hazard_rate;    /* PLAN.md §7.15; 0.0 si no aplica (sin riesgo de default) */
    double recovery_rate;  /* PLAN.md §7.15; 0.0 si no aplica */
} EngineMarketSnapshot;

typedef struct EnginePricingContext {
    double pricing_date;   /* metadato, sin aritmetica de calendario todavia (ver PLAN.md) */
    uint64_t n_paths;
    uint64_t n_steps;
    uint64_t seed;
} EnginePricingContext;

typedef struct EngineExecutionContext {
    const char* backend;    /* "cpu"/"gpu"/"auto" (case-insensitive); "auto" se resuelve al
                              * vuelo dentro de engine_abi_price, no hay handle que lo fije */
    const char* precision;  /* solo "fp64" aceptado hoy (PLAN.md §5.1) */
} EngineExecutionContext;

/* --- Calibracion (PLAN.md §7.14, generalizada en §7.18) ----------------------------------
 * EngineCalibrator es un handle opaco mas (mismo patron que EngineModel/EngineProduct): con
 * un segundo ICalibrator ya registrado (HullWhite2F, ademas de HullWhite1F) hay genericidad
 * real que ganar aqui, a diferencia de cuando solo existia uno (PLAN.md §7.14). El resultado
 * (EngineCalibrationResult) devuelve los parametros optimos como un array de EngineParam --
 * mismo bag de parametros que ya consume engine_abi_create_model -- para que se puedan pasar
 * directamente a engine_abi_create_model sin traduccion, cerrando el circulo Mercado ->
 * calibrar -> Modelo calibrado igual que ya hacen los tests de las cinco capas. */
typedef struct EngineCalibrationResult {
    EngineParam* optimal_params; /* array owned por la libreria, liberar con
                                   * engine_abi_free_calibration_result */
    size_t n_params;
    double rmse;
    int iterations;
    int converged; /* 0 o 1 */
} EngineCalibrationResult;

/* Calibra `calibrator` a `market` partiendo de `initial_guess` (mismo bag de parametros que
 * engine_abi_create_model: que claves hacen falta y cuales de ellas se calibran de verdad
 * depende de que calibrador sea -- ver engine::HullWhite1FCalibrator/HullWhite2FCalibrator y
 * engine_core::calibration para el detalle de cada uno). Devuelve 0 en exito (`*out_result`
 * queda relleno, liberar con engine_abi_free_calibration_result) o != 0 en error (calibrator
 * NULL, market NULL/vacio, o una clave de initial_guess que ese calibrador necesita y no
 * recibio -- ver engine_abi_last_error; `*out_result` queda a cero). `market->hazard_rate`/
 * `recovery_rate` se ignoran aqui (la calibracion no usa datos de credito). */
ENGINE_ABI_API int engine_abi_calibrate(
    const EngineCalibrator* calibrator,
    const EngineMarketSnapshot* market,
    const EngineParam* initial_guess,
    size_t n_initial_guess,
    EngineCalibrationResult* out_result
);
ENGINE_ABI_API void engine_abi_free_calibration_result(EngineCalibrationResult* result);

/* --- ENGINE.PRICE (PLAN.md §7.15) ---------------------------------------------------------
 * Sustituye por completo engine_abi_create_measure/engine_abi_evaluate: calcula un lote de
 * medidas nombradas (ver engine_abi_list_measures) de una vez sobre el mismo product/model/
 * market/pricing/execution, en vez de una medida a la vez con un Params generico. */

/* times/primary/secondary tienen longitud `len` (0 y NULL para las tres si la medida es
 * puramente escalar, ej. "PV"/"DV01"/"UnilateralCVA" -- ver has_scalar/scalar). Mismo shape
 * que la version pre-§7.15 de esta ABI, ahora anidado dentro de EnginePriceResultEntry. */
typedef struct EngineMeasureResult {
    double* times;
    double* primary;
    double* secondary;
    size_t len;
    int has_scalar;    /* 0 o 1 */
    double scalar;      /* valido solo si has_scalar == 1 */
} EngineMeasureResult;

typedef struct EnginePriceResultEntry {
    char* measure_name; /* copia owned por la libreria, mismo texto que se pidio */
    EngineMeasureResult result;
} EnginePriceResultEntry;

/* Devuelve 0 en exito (`*out_entries`/`*out_count` quedan rellenos, liberar con
 * engine_abi_free_price_results) o != 0 en error (parametros invalidos -- un nombre de
 * `measure_names` que no aparece en engine_abi_list_measures, model/product/execution
 * invalidos -- ver engine_abi_last_error; `*out_entries` queda NULL, `*out_count` a 0). Los
 * resultados se devuelven en el mismo orden que `measure_names`. */
ENGINE_ABI_API int engine_abi_price(
    const EngineProduct* product,
    const char** measure_names,
    size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceResultEntry** out_entries,
    size_t* out_count
);
ENGINE_ABI_API void engine_abi_free_price_results(EnginePriceResultEntry* entries, size_t count);

/* --- ENGINE.PRICE_BATCH / ENGINE.PRICE_MANY / ENGINE.PRICE_GRID (PLAN.md §7.17/§7.19) --------
 * Tres niveles de la API de calculo por lotes: price_batch (homogeneo -- todos los `products`
 * deben ser del mismo tipo registrado y, para IRSwap, compartir calendario y traer fixed_rate
 * explicito, sin use_par_rate) y price_many (heterogeneo -- agrupa internamente por tipo +
 * calendario y llama a price_batch por grupo, nunca falla por heterogeneidad) devuelven la
 * MISMA forma: una fila por trade con su indice explicito. price_grid explota products x
 * models x markets (PricingContext/ExecutionContext compartidos, no forman parte de la
 * rejilla), una fila por (trade, model, market).
 *
 * `products`/`models` son arrays de PUNTEROS a handles opacos ya creados (const EngineProduct*
 * const*, const EngineModel* const*) -- primera vez que esta ABI recibe un array de handles en
 * vez de uno solo, mismo patron array+count que el resto (EngineParam*, measure_names).
 * `markets` es un array de EngineMarketSnapshot por VALOR (struct plano, no handle). */
typedef struct EnginePriceBatchResultEntry {
    size_t trade_index;
    EnginePriceResultEntry* measures; /* array owned por la libreria, n_measures largo */
    size_t n_measures;
} EnginePriceBatchResultEntry;

/* Devuelve 0 en exito (`*out_entries`/`*out_count` rellenos, liberar con
 * engine_abi_free_price_batch_results) o != 0 en error (products vacio o con NULL, tipo de
 * producto no soportado para lote, calendarios distintos entre trades, algun trade con
 * use_par_rate, nombre de medida desconocido -- ver engine_abi_last_error). */
ENGINE_ABI_API int engine_abi_price_batch(
    const EngineProduct** products,
    size_t n_products,
    const char** measure_names,
    size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceBatchResultEntry** out_entries,
    size_t* out_count
);
/* Misma firma que engine_abi_price_batch; a diferencia de ella, acepta products de tipos/
 * calendarios distintos (los agrupa internamente) y nunca falla por heterogeneidad. */
ENGINE_ABI_API int engine_abi_price_many(
    const EngineProduct** products,
    size_t n_products,
    const char** measure_names,
    size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceBatchResultEntry** out_entries,
    size_t* out_count
);
ENGINE_ABI_API void engine_abi_free_price_batch_results(EnginePriceBatchResultEntry* entries, size_t count);

typedef struct EnginePriceGridResultEntry {
    size_t trade_index;
    size_t model_index;
    size_t market_index;
    EnginePriceResultEntry* measures;
    size_t n_measures;
} EnginePriceGridResultEntry;

/* Devuelve 0 en exito o != 0 en error (products/models/markets vacios, mismos errores que
 * engine_abi_price_many por cada combinacion model x market -- ver engine_abi_last_error). */
ENGINE_ABI_API int engine_abi_price_grid(
    const EngineProduct** products,
    size_t n_products,
    const char** measure_names,
    size_t n_measure_names,
    const EngineModel** models,
    size_t n_models,
    const EngineMarketSnapshot* markets,
    size_t n_markets,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceGridResultEntry** out_entries,
    size_t* out_count
);
ENGINE_ABI_API void engine_abi_free_price_grid_results(EnginePriceGridResultEntry* entries, size_t count);

/* --- ENGINE.ALL_GREEKS (PLAN_GREEKS.md §8.5/§9.3, Fase 9) --------------------------------
 * Barrido automatico de Greeks (engine::greeks::compute_all_greeks): enumera los RiskFactor
 * candidatos de `model`/`market` para `metric_name` y calcula todos los que apliquen, sin que
 * el llamante enumere spot/rate/sigma/curva/credito/tiempo a mano (mismo motivo que
 * engine_abi_price no repite create_measure por medida). Un unico "Greek" suelto (una
 * combinacion metric/risk_factor/order/method concreta) sigue alcanzable con
 * engine_abi_price(product, {"Greek"}, ...) pasando esos campos en `measure_params` -- esta
 * funcion es el barrido, no un reemplazo de esa via. */

/* Analogo a EnginePriceResultEntry pero para una fila de GreeksReport::greeks: a diferencia
 * del sketch de PLAN_GREEKS.md §9.3 (que solo preveia un `value` escalar), `result` es un
 * EngineMeasureResult completo (mismo tipo que ya usa EnginePriceResultEntry) porque
 * GreekResult puede llevar un perfil temporal (times/primary) ademas de, o en vez de, un
 * escalar (PLAN_GREEKS.md §11 Fase 2) -- reusar EngineMeasureResult evita un tercer struct de
 * resultado con el mismo shape. */
typedef struct EngineGreekResultEntry {
    char* risk_factor;   /* copia owned, ver engine::greeks::to_string(RiskFactor) */
    char* method_used;   /* "auto" nunca aparece aqui: siempre el metodo REALMENTE ejecutado */
    char* measure;        /* "RiskNeutralQ" | "PhysicalP" | "DeterministicScenario" */
    EngineMeasureResult result;
    int has_std_error;    /* 0 o 1 */
    double std_error;      /* valido solo si has_std_error == 1 */
    int has_bump;          /* 0 o 1 -- ausente si method_used es "pathwise"/"aad" */
    double bump_used;      /* valido solo si has_bump == 1 */
} EngineGreekResultEntry;

/* Devuelve 0 en exito (`*out_greeks`/`*out_n_greeks`/`*out_skipped`/`*out_n_skipped` quedan
 * rellenos, liberar con engine_abi_free_greeks_report) o != 0 en error (product/model/market/
 * pricing/execution NULL, metric_name desconocido para Registry<IMeasure> -- ver
 * engine_abi_last_error; en ese caso `*out_greeks`/`*out_skipped` quedan NULL y los counts a
 * 0). Un candidato individual que no aplique (p.ej. credit.hazard_rate sobre una metrica sin
 * credito, o time.theta sobre una metrica todavia no cableada) NUNCA hace fallar la llamada
 * entera: cae en `*out_skipped` con el motivo (PLAN_GREEKS.md §8.5, "best effort"). */
ENGINE_ABI_API int engine_abi_all_greeks(
    const EngineProduct* product,
    const char* metric_name,
    const EngineParam* metric_params,
    size_t n_metric_params,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    int include_curve_buckets,
    int include_second_order,
    EngineGreekResultEntry** out_greeks,
    size_t* out_n_greeks,
    char*** out_skipped,
    size_t* out_n_skipped
);
ENGINE_ABI_API void engine_abi_free_greeks_report(
    EngineGreekResultEntry* greeks, size_t n_greeks, char** skipped, size_t n_skipped
);

/* --- ENGINE.HESSIAN / ENGINE.HVP (PLAN_BACKWARD.md §8.3/§9 Fase 1-3) ---------------------
 * Mismo molde exacto que EngineGreekResultEntry/engine_abi_all_greeks de arriba: struct
 * owned-copy + funcion que rellena out_entries/out_count + free. Hessiano local de un trade
 * (engine::greeks::compute_hessian) y producto Hessiano-vector H*v (engine::greeks::
 * compute_hvp) -- ver el doc-comment de esas dos funciones en engine/greeks.hpp para la tabla
 * de capacidades (GBM/GBM_P via likelihood ratio, HullWhite1F/2F via forward-over-forward) y el
 * criterio "mejor esfuerzo" (una combinacion no soportada nunca hace fallar la llamada entera:
 * cae en *out_skipped, igual que engine_abi_all_greeks). */

typedef struct EngineHessianEntry {
    char* risk_factor_i;   /* copia owned, ver engine::greeks::to_string(RiskFactor) */
    char* risk_factor_j;   /* factor_i == factor_j -> entrada diagonal (Gamma/Volga) */
    double value;
    int has_std_error;     /* 0 o 1 -- valido solo si method_used == "likelihood_ratio_hessian" */
    double std_error;      /* valido solo si has_std_error == 1 */
    char* method_used;     /* copia owned, ver engine::greeks::to_string(GreekMethod) */
    char* measure;         /* copia owned, ver engine::greeks::to_string(ProbabilityMeasure) */
} EngineHessianEntry;

/* Devuelve 0 en exito (*out_entries/*out_n_entries/*out_skipped/*out_n_skipped quedan rellenos,
 * liberar con engine_abi_free_hessian) o != 0 en error (product/model/market/pricing/execution/
 * metric_name NULL -- ver engine_abi_last_error; en ese caso *out_entries/*out_skipped quedan
 * NULL y los counts a 0). risk_factors/n_risk_factors: NULL/0 = enumeracion automatica de los
 * factores soportados por (modelo, metrica) (mismo criterio que factors={} en
 * engine::greeks::compute_hessian); no vacio = solo los pares formables con esos factores. Una
 * combinacion (modelo, metrica) o un factor no soportado NUNCA hace fallar la llamada entera:
 * cae en *out_skipped con el motivo, igual que engine_abi_all_greeks. */
ENGINE_ABI_API int engine_abi_hessian(
    const EngineProduct* product,
    const char* metric_name,
    const EngineParam* metric_params,
    size_t n_metric_params,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    const char** risk_factors,
    size_t n_risk_factors,
    EngineHessianEntry** out_entries,
    size_t* out_n_entries,
    char*** out_skipped,
    size_t* out_n_skipped
);
ENGINE_ABI_API void engine_abi_free_hessian(
    EngineHessianEntry* entries, size_t n_entries, char** skipped, size_t n_skipped
);

typedef struct EngineHvpComponent {
    char* risk_factor;   /* copia owned, ver engine::greeks::to_string(RiskFactor) */
    double value;        /* componente de H*v en la posicion de `risk_factor` */
    char* method_used;   /* copia owned, ver engine::greeks::to_string(GreekMethod) */
} EngineHvpComponent;

/* Devuelve 0 en exito (*out_components/*out_n_components/*out_skipped/*out_n_skipped quedan
 * rellenos, liberar con engine_abi_free_hvp) o != 0 en error (product/model/market/pricing/
 * execution/metric_name NULL, o direction_factors/direction_weights NULL/0 -- a diferencia de
 * risk_factors en engine_abi_hessian, aqui SIEMPRE son obligatorios, sin el caso "0 =
 * automatico": un HVP sin direccion no significa nada). direction_factors[i] se empareja con
 * direction_weights[i], mismo orden -- ver engine::greeks::compute_hvp. Un factor cuya fila del
 * Hessiano quedo incompleta cae en *out_skipped, nunca se inventa un 0.0 silencioso. */
ENGINE_ABI_API int engine_abi_hvp(
    const EngineProduct* product,
    const char* metric_name,
    const EngineParam* metric_params,
    size_t n_metric_params,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    const char** direction_factors,
    const double* direction_weights,
    size_t n_direction,
    EngineHvpComponent** out_components,
    size_t* out_n_components,
    char*** out_skipped,
    size_t* out_n_skipped
);
ENGINE_ABI_API void engine_abi_free_hvp(
    EngineHvpComponent* components, size_t n_components, char** skipped, size_t n_skipped
);

/* --- ENGINE.PORTFOLIO (PLAN_BACKWARD.md §6.4/§9 Fase 6) ----------------------------------
 * `EnginePortfolio` es un handle opaco mas, mismo molde EXACTO que EngineModel/EngineProduct
 * (crear con engine_abi_create_portfolio, liberar exactamente una vez con
 * engine_abi_free_portfolio). Conceptualmente, una lista de trades (EngineProduct) valorados/
 * arriesgados juntos bajo un UNICO EngineModel/EngineMarketSnapshot por llamada -- alcance
 * explicitamente minimo (PLAN_BACKWARD.md §6.4/§12: sin netting/colateral/multi-moneda/
 * multi-modelo). engine_abi_portfolio_add_trade NO transfiere la propiedad de `trade` (a
 * diferencia de engine_abi_create_portfolio/engine_abi_free_portfolio sobre el propio
 * EnginePortfolio): `trade` sigue siendo responsabilidad de quien lo creo, y se puede seguir
 * usando (engine_abi_price, otro EnginePortfolio, ...) o liberar con engine_abi_free_product
 * en cualquier momento DESPUES de anadirlo a un Portfolio sin invalidar este ultimo -- ver el
 * comentario de `EngineProduct` en abi.cpp (shared_ptr, no unique_ptr) para el porque.
 *
 * engine_abi_portfolio_price/_hessian/_hvp reutilizan los MISMOS structs de salida owned-copy
 * que ya usan engine_abi_price_many/engine_abi_hessian/engine_abi_hvp
 * (EnginePriceBatchResultEntry/EngineHessianEntry/EngineHvpComponent) -- un Portfolio no
 * necesita un formato de tabla nuevo, el resultado tiene la MISMA forma que la version de un
 * solo trade/lista de trades. Se liberan con esos mismos engine_abi_free_price_batch_results/
 * engine_abi_free_hessian/engine_abi_free_hvp ya existentes, sin funciones _free nuevas. */

typedef struct EnginePortfolio EnginePortfolio;

ENGINE_ABI_API EnginePortfolio* engine_abi_create_portfolio(void);
/* No-op silencioso (ver engine_abi_last_error) si portfolio/trade son NULL -- sin valor de
 * retorno que comprobar (mismo molde que el boceto original de PLAN_BACKWARD.md §6.4), pero el
 * detalle de un NULL queda igualmente en engine_abi_last_error() para quien quiera
 * comprobarlo. */
ENGINE_ABI_API void engine_abi_portfolio_add_trade(EnginePortfolio* portfolio, const EngineProduct* trade);
ENGINE_ABI_API size_t engine_abi_portfolio_size(const EnginePortfolio* portfolio);
ENGINE_ABI_API void engine_abi_free_portfolio(EnginePortfolio* portfolio);

/* Envoltorio fino sobre engine::Portfolio::price (-> engine::price_many): mismos parametros que
 * engine_abi_price_many, sustituyendo products/n_products por un unico EnginePortfolio*. Devuelve
 * 0 en exito (*out_entries/*out_count rellenos, liberar con
 * engine_abi_free_price_batch_results) o != 0 en error (portfolio/model/market/pricing/execution
 * NULL, portfolio vacio, nombre de medida desconocido -- ver engine_abi_last_error). */
ENGINE_ABI_API int engine_abi_portfolio_price(
    const EnginePortfolio* portfolio,
    const char** measure_names,
    size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceBatchResultEntry** out_entries,
    size_t* out_count
);

/* Envoltorio sobre engine::Portfolio::hessian (suma, trade a trade, los HessianReport de
 * engine::greeks::compute_hessian -- ver el doc-comment de esa funcion y de engine/portfolio.hpp
 * para el criterio de interseccion/skip: un par que no aparezca en TODOS los trades del
 * portfolio nunca se suma como si el trade que falta aportara 0.0, va a *out_skipped
 * nombrando el par y el/los indice(s) de trade exacto(s)). Mismos parametros que
 * engine_abi_hessian, sustituyendo product por portfolio. Devuelve 0 en exito (liberar con
 * engine_abi_free_hessian) o != 0 en error (portfolio/model/market/pricing/execution/
 * metric_name NULL -- ver engine_abi_last_error). */
ENGINE_ABI_API int engine_abi_portfolio_hessian(
    const EnginePortfolio* portfolio,
    const char* metric_name,
    const EngineParam* metric_params,
    size_t n_metric_params,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    const char** risk_factors,
    size_t n_risk_factors,
    EngineHessianEntry** out_entries,
    size_t* out_n_entries,
    char*** out_skipped,
    size_t* out_n_skipped
);

/* Envoltorio sobre engine::Portfolio::hvp (suma, trade a trade, los HvpReport de
 * engine::greeks::compute_hvp -- mismo criterio de interseccion/skip que
 * engine_abi_portfolio_hessian, pero por factor en vez de por par). Mismos parametros que
 * engine_abi_hvp, sustituyendo product por portfolio. Devuelve 0 en exito (liberar con
 * engine_abi_free_hvp) o != 0 en error (portfolio/model/market/pricing/execution/metric_name
 * NULL, o direction_factors/direction_weights NULL/0 -- siempre obligatorios, sin auto-
 * enumeracion, igual que engine_abi_hvp -- ver engine_abi_last_error). */
ENGINE_ABI_API int engine_abi_portfolio_hvp(
    const EnginePortfolio* portfolio,
    const char* metric_name,
    const EngineParam* metric_params,
    size_t n_metric_params,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    const char** direction_factors,
    const double* direction_weights,
    size_t n_direction,
    EngineHvpComponent** out_components,
    size_t* out_n_components,
    char*** out_skipped,
    size_t* out_n_skipped
);

ENGINE_ABI_API int engine_abi_is_gpu_backend_available(void);

/* --- Errores --------------------------------------------------------------------------
 * Mensaje de la ultima llamada de ESTE HILO (thread-local) a una funcion de este header que
 * fallo; cadena vacia si la ultima llamada tuvo exito. Misma convencion de buffer/longitud
 * que snprintf: escribe hasta buffer_len bytes (incluido el NUL final) y devuelve la
 * longitud real del mensaje sin contar el NUL -- si el valor devuelto es >= buffer_len, el
 * contenido de `buffer` fue truncado. `buffer`/`buffer_len` pueden ser NULL/0 para solo
 * consultar la longitud necesaria. */
ENGINE_ABI_API size_t engine_abi_last_error(char* buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_ABI_H */
