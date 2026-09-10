/* engine/abi.h -- API universal en C ABI (PLAN.md Fase 6, §5.5).
 *
 * A diferencia del resto de cpp/engine/include/engine/*.hpp (C++, consumidos directamente
 * por clients/python via nanobind y clients/excel via el bridge Excel), este header es
 * deliberadamente C puro: sin plantillas, sin std::string/std::vector/excepciones en la
 * frontera, para que cualquier lenguaje con FFI a C (Julia via ccall, .NET via P/Invoke, Go
 * via cgo, o C/C++ directamente) pueda enlazarlo sin un compilador de C++ ni conocer nada de
 * cxx/nanobind. No es una cuarta API distinta (PLAN.md §5.5): es la misma superficie que ya
 * consumen Python/Excel (Registries/register_builtins/Registry<T>::create/engine::calc, mas
 * Market/PricingContext/ExecutionContext de PLAN.md §7.15) expresada en C ABI.
 *
 * Convenciones de esta ABI (validas para toda funcion de este header salvo que se diga lo
 * contrario):
 *   - Los handles (EngineModel*, EngineProduct*) son opacos y de ownership explicito: quien
 *     los crea (engine_abi_create_*) debe liberarlos (engine_abi_free_*) exactamente una vez.
 *     Nunca se debe hacer free()/delete directamente sobre ellos. Market/PricingContext/
 *     ExecutionContext NO son handles (no son polimorficos, no tienen ciclo de vida que
 *     gestionar): se pasan como structs planos por valor/puntero-const directamente a
 *     engine_abi_calc, igual que EngineParam.
 *   - Las cadenas de entrada (const char*) son UTF-8, terminadas en NUL, y no se retienen mas
 *     alla de la duracion de la llamada (se copian internamente si hace falta guardarlas).
 *   - Los structs planos que devuelven datos owned por esta libreria (EngineMeasureResult,
 *     EngineCalcResultEntry, las listas de engine_abi_list_*) se liberan con su
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
 * engine_abi_create_measure/engine_abi_evaluate (sustituidos por engine_abi_calc), se
 * elimina engine_abi_set_compute_backend/engine_abi_get_compute_backend (el backend pasa a
 * ser un campo de EngineExecutionContext, no un estado global), y cambia el layout de
 * EngineMarketSnapshot (gana hazard_rate/recovery_rate). Comprobar antes de asumir el layout
 * de cualquier struct de este header en un binario compilado contra una version futura. */
ENGINE_ABI_API int engine_abi_version(void);

/* --- Handles opacos (solo Model/Product: polimorficos, con ciclo de vida) ---------------- */

typedef struct EngineModel EngineModel;
typedef struct EngineProduct EngineProduct;

/* --- Parametros (PLAN.md §5.5: "structs planos / punteros + longitud", igual bag de
 * parametros que engine::Params -- ver engine/params.hpp -- expresado sin std::variant) --- */

typedef enum EngineParamKind {
    ENGINE_PARAM_DOUBLE = 0,
    ENGINE_PARAM_VECTOR = 1,
    ENGINE_PARAM_BOOL = 2
} EngineParamKind;

typedef struct EngineParam {
    const char* key;      /* UTF-8, NUL-terminado; no se retiene tras la llamada */
    EngineParamKind kind;
    double scalar;         /* valido si kind es ENGINE_PARAM_DOUBLE o ENGINE_PARAM_BOOL (0.0/1.0) */
    const double* values;  /* valido (no NULL) si kind es ENGINE_PARAM_VECTOR */
    size_t count;           /* longitud de values; valido si kind es ENGINE_PARAM_VECTOR */
} EngineParam;

/* --- Listado de modelos/productos registrados (PLAN.md §5.4) y medidas de ENGINE.CALC
 * (PLAN.md §7.15: engine_abi_list_measures devuelve los nombres de cara al usuario -- "PV",
 * "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA" -- no los nombres registrados en
 * Registry<IMeasure>, que quedan como detalle interno de extension) -----------------------
 * *out_names queda apuntando a un array de `size_t` cadenas C (o NULL si el array esta
 * vacio); liberar siempre con engine_abi_free_string_list, incluso si el count devuelto es 0
 * y *out_names es NULL (no hace falta comprobarlo, engine_abi_free_string_list acepta NULL). */
ENGINE_ABI_API size_t engine_abi_list_models(const char*** out_names);
ENGINE_ABI_API size_t engine_abi_list_products(const char*** out_names);
ENGINE_ABI_API size_t engine_abi_list_measures(const char*** out_names);
ENGINE_ABI_API void engine_abi_free_string_list(const char** names, size_t count);

/* --- Creacion / liberacion de modelos y productos ----------------------------------------
 * Devuelven NULL si `name` no esta registrado o los parametros no son validos para ese tipo
 * (ver engine_abi_last_error). */
ENGINE_ABI_API EngineModel* engine_abi_create_model(
    const char* name, const EngineParam* params, size_t n_params
);
ENGINE_ABI_API EngineProduct* engine_abi_create_product(
    const char* name, const EngineParam* params, size_t n_params
);

/* Liberar exactamente una vez cada handle devuelto por un engine_abi_create_*; NULL se
 * ignora (como free()). */
ENGINE_ABI_API void engine_abi_free_model(EngineModel* model);
ENGINE_ABI_API void engine_abi_free_product(EngineProduct* product);

/* --- Market / PricingContext / ExecutionContext (PLAN.md §7.15) -------------------------
 * Structs planos, no handles: se construyen y se pasan directamente a engine_abi_calibrate_
 * hull_white/engine_abi_calc, sin creacion/liberacion propia. */

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
                              * vuelo dentro de engine_abi_calc, no hay handle que lo fije */
    const char* precision;  /* solo "fp64" aceptado hoy (PLAN.md §5.1) */
} EngineExecutionContext;

/* --- Calibracion (PLAN.md §7.14) ---------------------------------------------------------
 * Sin handle de calibrador ni Registry<ICalibrator> a este nivel (a diferencia de model/
 * product): con un solo calibrador implementado hoy (HullWhite1F) no hay genericidad
 * real que ganar todavia con un engine_abi_create_calibrator/engine_abi_calibrate genericos
 * -- se anadira cuando exista un segundo. La capa C++ (engine::ICalibrator, PLAN.md §7.14) SI
 * es generica ya, esta funcion es su unica traduccion a esta ABI por ahora. */
typedef struct EngineHullWhiteCalibration {
    double a;
    double b;
    double sigma; /* no se calibra: se devuelve tal cual se paso, ver engine_abi_calibrate_hull_white */
    double r0;    /* no se calibra: se devuelve tal cual se paso */
    double rmse;
    int iterations;
    int converged; /* 0 o 1 */
} EngineHullWhiteCalibration;

/* Calibra a/b de HullWhite1F a `market` por minimos cuadrados sobre el factor de descuento,
 * partiendo de (initial_a, initial_b); sigma/r0 no se calibran (ver engine_core::calibration
 * en el core Rust para el porque: sigma solo entra en el precio del bono cero-cupon como un
 * efecto de segundo orden, mal identificado contra unicamente una curva de descuento).
 * Devuelve 0 en exito (`*out_result` queda relleno) o != 0 en error (parametros invalidos --
 * `market->count == 0`, pillars no creciente, initial_a <= 0 -- ver engine_abi_last_error;
 * `*out_result` queda a cero). No hace falta liberar `*out_result`: son todo campos planos,
 * sin punteros owned por la libreria. `market->hazard_rate`/`recovery_rate` se ignoran aqui
 * (la calibracion no usa datos de credito). */
ENGINE_ABI_API int engine_abi_calibrate_hull_white(
    const EngineMarketSnapshot* market,
    double initial_a,
    double initial_b,
    double sigma,
    double r0,
    EngineHullWhiteCalibration* out_result
);

/* --- ENGINE.CALC (PLAN.md §7.15) ---------------------------------------------------------
 * Sustituye por completo engine_abi_create_measure/engine_abi_evaluate: calcula un lote de
 * medidas nombradas (ver engine_abi_list_measures) de una vez sobre el mismo product/model/
 * market/pricing/execution, en vez de una medida a la vez con un Params generico. */

/* times/primary/secondary tienen longitud `len` (0 y NULL para las tres si la medida es
 * puramente escalar, ej. "PV"/"DV01"/"UnilateralCVA" -- ver has_scalar/scalar). Mismo shape
 * que la version pre-§7.15 de esta ABI, ahora anidado dentro de EngineCalcResultEntry. */
typedef struct EngineMeasureResult {
    double* times;
    double* primary;
    double* secondary;
    size_t len;
    int has_scalar;    /* 0 o 1 */
    double scalar;      /* valido solo si has_scalar == 1 */
} EngineMeasureResult;

typedef struct EngineCalcResultEntry {
    char* measure_name; /* copia owned por la libreria, mismo texto que se pidio */
    EngineMeasureResult result;
} EngineCalcResultEntry;

/* Devuelve 0 en exito (`*out_entries`/`*out_count` quedan rellenos, liberar con
 * engine_abi_free_calc_results) o != 0 en error (parametros invalidos -- un nombre de
 * `measure_names` que no aparece en engine_abi_list_measures, model/product/execution
 * invalidos -- ver engine_abi_last_error; `*out_entries` queda NULL, `*out_count` a 0). Los
 * resultados se devuelven en el mismo orden que `measure_names`. */
ENGINE_ABI_API int engine_abi_calc(
    const EngineProduct* product,
    const char** measure_names,
    size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EngineCalcResultEntry** out_entries,
    size_t* out_count
);
ENGINE_ABI_API void engine_abi_free_calc_results(EngineCalcResultEntry* entries, size_t count);

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
