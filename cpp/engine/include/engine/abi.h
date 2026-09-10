/* engine/abi.h -- API universal en C ABI (PLAN.md Fase 6, §5.5).
 *
 * A diferencia del resto de cpp/engine/include/engine/*.hpp (C++, consumidos directamente
 * por clients/python via nanobind y clients/excel via el bridge Excel), este header es
 * deliberadamente C puro: sin plantillas, sin std::string/std::vector/excepciones en la
 * frontera, para que cualquier lenguaje con FFI a C (Julia via ccall, .NET via P/Invoke, Go
 * via cgo, o C/C++ directamente) pueda enlazarlo sin un compilador de C++ ni conocer nada de
 * cxx/nanobind. No es una cuarta API distinta (PLAN.md §5.5): es la misma superficie que ya
 * consumen Python/Excel (Registries/register_builtins/Registry<T>::create/IMeasure::evaluate,
 * mas la seleccion de backend de PLAN.md §7.12) expresada en C ABI.
 *
 * Convenciones de esta ABI (validas para toda funcion de este header salvo que se diga lo
 * contrario):
 *   - Los handles (EngineModel*, EngineProduct*, EngineMeasure*) son opacos y de ownership
 *     explicito: quien los crea (engine_abi_create_*) debe liberarlos (engine_abi_free_*)
 *     exactamente una vez. Nunca se debe hacer free()/delete directamente sobre ellos.
 *   - Las cadenas de entrada (const char*) son UTF-8, terminadas en NUL, y no se retienen mas
 *     alla de la duracion de la llamada (se copian internamente si hace falta guardarlas).
 *   - Los structs planos que devuelven datos owned por esta libreria (EngineMeasureResult,
 *     las listas de engine_abi_list_*) se liberan con su engine_abi_free_* correspondiente,
 *     nunca con el free()/delete del lenguaje que consume la ABI (los allocators pueden no
 *     coincidir entre el .dll/.so de este motor y el runtime del consumidor).
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

/* Version de esta ABI (PLAN.md §5.5). Empieza en 1; comprobar antes de asumir el layout de
 * cualquier struct de este header en un binario compilado contra una version futura. */
ENGINE_ABI_API int engine_abi_version(void);

/* --- Handles opacos --------------------------------------------------------------------- */

typedef struct EngineModel EngineModel;
typedef struct EngineProduct EngineProduct;
typedef struct EngineMeasure EngineMeasure;

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

/* --- Listado de modelos/productos/medidas registrados (PLAN.md §5.4) --------------------
 * *out_names queda apuntando a un array de `size_t` cadenas C (o NULL si el array esta
 * vacio); liberar siempre con engine_abi_free_string_list, incluso si el count devuelto es 0
 * y *out_names es NULL (no hace falta comprobarlo, engine_abi_free_string_list acepta NULL). */
ENGINE_ABI_API size_t engine_abi_list_models(const char*** out_names);
ENGINE_ABI_API size_t engine_abi_list_products(const char*** out_names);
ENGINE_ABI_API size_t engine_abi_list_measures(const char*** out_names);
ENGINE_ABI_API void engine_abi_free_string_list(const char** names, size_t count);

/* --- Creacion / liberacion de modelos, productos y medidas -------------------------------
 * Devuelven NULL si `name` no esta registrado o los parametros no son validos para ese tipo
 * (ver engine_abi_last_error). engine_abi_create_measure no toma parametros: los parametros
 * de una medida (monitoring_times, n_paths, seed, ...) se pasan en engine_abi_evaluate. */
ENGINE_ABI_API EngineModel* engine_abi_create_model(
    const char* name, const EngineParam* params, size_t n_params
);
ENGINE_ABI_API EngineProduct* engine_abi_create_product(
    const char* name, const EngineParam* params, size_t n_params
);
ENGINE_ABI_API EngineMeasure* engine_abi_create_measure(const char* name);

/* Liberar exactamente una vez cada handle devuelto por un engine_abi_create_*; NULL se
 * ignora (como free()). */
ENGINE_ABI_API void engine_abi_free_model(EngineModel* model);
ENGINE_ABI_API void engine_abi_free_product(EngineProduct* product);
ENGINE_ABI_API void engine_abi_free_measure(EngineMeasure* measure);

/* --- Evaluacion (engine::IMeasure::evaluate, ver engine/measure.hpp) ---------------------
 * times/primary/secondary tienen longitud `len` (0 y NULL para las tres si la medida no
 * produce perfil temporal, ej. UnilateralCVA -- ver has_scalar/scalar para su agregado). */
typedef struct EngineMeasureResult {
    double* times;
    double* primary;
    double* secondary;
    size_t len;
    int has_scalar;    /* 0 o 1 */
    double scalar;      /* valido solo si has_scalar == 1 */
} EngineMeasureResult;

/* Devuelve 0 en exito (`*out_result` queda relleno y debe liberarse con
 * engine_abi_free_measure_result) o != 0 en error (`*out_result` queda a cero, NO hace falta
 * ni se debe llamar a engine_abi_free_measure_result en ese caso; ver engine_abi_last_error). */
ENGINE_ABI_API int engine_abi_evaluate(
    const EngineMeasure* measure,
    const EngineModel* model,
    const EngineProduct* product,
    const EngineParam* params,
    size_t n_params,
    EngineMeasureResult* out_result
);
ENGINE_ABI_API void engine_abi_free_measure_result(EngineMeasureResult* result);

/* --- Backend de computo (PLAN.md §7.12) --------------------------------------------------
 * Mismo estado global de proceso que engine::set_compute_backend/compute_backend_name (ver
 * engine/engine.hpp): seleccionarlo aqui afecta tambien a Python/Excel si comparten proceso
 * (no es el caso salvo un embedding deliberado), y viceversa. */
ENGINE_ABI_API int engine_abi_set_compute_backend(const char* name); /* 1 ok, 0 rechazado */
/* Escribe hasta buffer_len bytes (incluido el NUL final) en `buffer` y devuelve la longitud
 * real del nombre sin contar el NUL (semantica de snprintf: si el valor devuelto es >=
 * buffer_len, el contenido de `buffer` fue truncado). `buffer`/`buffer_len` pueden ser
 * NULL/0 para solo consultar la longitud necesaria. */
ENGINE_ABI_API size_t engine_abi_get_compute_backend(char* buffer, size_t buffer_len);
ENGINE_ABI_API int engine_abi_is_gpu_backend_available(void);

/* --- Errores --------------------------------------------------------------------------
 * Mensaje de la ultima llamada de ESTE HILO (thread-local) a una funcion de este header que
 * fallo; cadena vacia si la ultima llamada tuvo exito. Misma convencion de buffer/longitud
 * que engine_abi_get_compute_backend. */
ENGINE_ABI_API size_t engine_abi_last_error(char* buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_ABI_H */
