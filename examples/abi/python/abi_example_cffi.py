"""Ejemplo de Python consumiendo engine/abi.h (PLAN.md Fase 6, §5.5/§7.13) via `cffi`, en modo
ABI (`ffi.dlopen`, sin compilar una extension C -- a diferencia del modo API de cffi, que sí
compilaría un `.pyd`/`.so` propio y se parecería mas a `clients/python`). Mismo recorrido que
`abi_example_ctypes.py`: comparar ambos ficheros lado a lado es la forma mas directa de ver la
diferencia entre las dos librerias de FFI mas comunes de Python.

Frente a ctypes (ver abi_example_ctypes.py): `ffi.cdef(...)` acepta declaraciones en sintaxis
C casi literal (la version de abi.h sin macros de exportacion ni directivas de preprocesador,
que cffi no soporta) en vez de tener que traducir cada struct campo a campo a
`ctypes.Structure`/`ctypes.POINTER(...)` -- menos codigo repetido, mas cerca del propio
header. Requiere `pip install cffi` (no es libreria estandar, a diferencia de ctypes).

Requiere haber compilado antes el arbol CMake de este repo (ver examples/abi/README.md): por
defecto busca engine_abi.dll/.so en <repo>/build/cpp/engine, o en la ruta que indique la
variable de entorno ENGINE_ABI_LIB_DIR/ENGINE_ABI_LIB_PATH (misma convencion que
abi_example_ctypes.py).

    pip install cffi
    python examples/abi/python/abi_example_cffi.py
"""

import os
import sys
from pathlib import Path

from cffi import FFI

# Traduccion de engine/abi.h a cdef de cffi: mismas declaraciones que el header, sin
# ENGINE_ABI_API/__declspec, sin el guard #ifdef __cplusplus (cdef no soporta directivas de
# preprocesador) y sin comentarios de documentacion (cdef los rechaza si no son /* ... */
# balanceados en cada declaracion; se omiten aqui, el contrato ya esta documentado en abi.h).
_CDEF = """
    int engine_abi_version(void);

    typedef struct EngineModel EngineModel;
    typedef struct EngineProduct EngineProduct;
    typedef struct EngineMeasure EngineMeasure;

    typedef enum EngineParamKind {
        ENGINE_PARAM_DOUBLE = 0,
        ENGINE_PARAM_VECTOR = 1,
        ENGINE_PARAM_BOOL = 2
    } EngineParamKind;

    typedef struct EngineParam {
        const char* key;
        EngineParamKind kind;
        double scalar;
        const double* values;
        size_t count;
    } EngineParam;

    size_t engine_abi_list_models(const char*** out_names);
    size_t engine_abi_list_products(const char*** out_names);
    size_t engine_abi_list_measures(const char*** out_names);
    void engine_abi_free_string_list(const char** names, size_t count);

    EngineModel* engine_abi_create_model(const char* name, const EngineParam* params, size_t n_params);
    EngineProduct* engine_abi_create_product(const char* name, const EngineParam* params, size_t n_params);
    EngineMeasure* engine_abi_create_measure(const char* name);

    void engine_abi_free_model(EngineModel* model);
    void engine_abi_free_product(EngineProduct* product);
    void engine_abi_free_measure(EngineMeasure* measure);

    typedef struct EngineMeasureResult {
        double* times;
        double* primary;
        double* secondary;
        size_t len;
        int has_scalar;
        double scalar;
    } EngineMeasureResult;

    int engine_abi_evaluate(
        const EngineMeasure* measure,
        const EngineModel* model,
        const EngineProduct* product,
        const EngineParam* params,
        size_t n_params,
        EngineMeasureResult* out_result
    );
    void engine_abi_free_measure_result(EngineMeasureResult* result);

    int engine_abi_set_compute_backend(const char* name);
    size_t engine_abi_get_compute_backend(char* buffer, size_t buffer_len);
    int engine_abi_is_gpu_backend_available(void);

    size_t engine_abi_last_error(char* buffer, size_t buffer_len);
"""


def _default_lib_dir() -> Path:
    # examples/abi/python/abi_example_cffi.py -> <repo>/build/cpp/engine
    return Path(__file__).resolve().parents[3] / "build" / "cpp" / "engine"


def _library_path() -> Path:
    override = os.environ.get("ENGINE_ABI_LIB_PATH")
    if override:
        return Path(override)

    lib_dir = Path(os.environ.get("ENGINE_ABI_LIB_DIR", _default_lib_dir()))
    name = "engine_abi.dll" if sys.platform == "win32" else "libengine_abi.so"
    if sys.platform == "darwin":
        name = "libengine_abi.dylib"
    return lib_dir / name


def load_engine_abi():
    path = _library_path()
    if not path.exists():
        raise FileNotFoundError(
            f"No se encontro {path}. Compila el arbol CMake de este repo primero "
            "(ver examples/abi/README.md), o define ENGINE_ABI_LIB_PATH/ENGINE_ABI_LIB_DIR."
        )
    ffi = FFI()
    ffi.cdef(_CDEF)
    lib = ffi.dlopen(str(path))
    return ffi, lib


def last_error(ffi, lib) -> str:
    buffer = ffi.new("char[]", 512)
    length = lib.engine_abi_last_error(buffer, len(buffer))
    return bytes(ffi.buffer(buffer, min(length, len(buffer)))).decode("utf-8", errors="replace")


def get_compute_backend(ffi, lib) -> str:
    buffer = ffi.new("char[]", 16)
    length = lib.engine_abi_get_compute_backend(buffer, len(buffer))
    return bytes(ffi.buffer(buffer, min(length, len(buffer)))).decode("ascii")


ENGINE_PARAM_DOUBLE = 0
ENGINE_PARAM_VECTOR = 1
# ENGINE_PARAM_BOOL = 2  -- no usado en este ejemplo (ningun parametro bool en el caso base)


def scalar_param(ffi, keepalive, key: bytes, value: float) -> dict:
    key_buf = ffi.new("char[]", key)
    keepalive.append(key_buf)  # el struct solo guarda el puntero, no una copia -- ver abajo
    return {"key": key_buf, "kind": ENGINE_PARAM_DOUBLE, "scalar": value, "values": ffi.NULL, "count": 0}


def vector_param(ffi, keepalive, key: bytes, values) -> dict:
    key_buf = ffi.new("char[]", key)
    values_buf = ffi.new("double[]", values)
    keepalive += [key_buf, values_buf]
    return {"key": key_buf, "kind": ENGINE_PARAM_VECTOR, "scalar": 0.0, "values": values_buf, "count": len(values)}


def main() -> None:
    ffi, lib = load_engine_abi()
    print("engine_abi_version() =", lib.engine_abi_version())

    # --- list_models: confirma que HullWhite1F esta registrado -----------------------------
    # ffi.new("T *") reserva UNA celda de tipo T y devuelve un puntero a ella: para que el
    # resultado sea "const char***" (lo que pide out_names), T debe ser "const char**", así
    # que hacen falta las tres estrellas.
    names_ptr = ffi.new("const char ***")
    count = lib.engine_abi_list_models(names_ptr)
    names_array = names_ptr[0]
    names = [ffi.string(names_array[i]).decode("utf-8") for i in range(count)]
    lib.engine_abi_free_string_list(names_array, count)
    assert "HullWhite1F" in names, f"HullWhite1F no aparece en engine_abi_list_models: {names}"

    # `keepalive` mantiene vivos los buffers de cffi (claves y arrays de doubles) mientras se
    # usan sus punteros dentro de un EngineParam -- cffi no los retiene automaticamente mas
    # alla de la expresion en la que se crean, a diferencia de un ctypes.Structure ya
    # construido (ver abi_example_ctypes.py). Todos duran hasta el final de main().
    keepalive: list = []

    # --- Caso base: IRS 5y anual a la par bajo Hull-White 1F (PLAN.md §5.2) ----------------
    hw_params = ffi.new(
        "EngineParam[]",
        [
            scalar_param(ffi, keepalive, b"a", 0.1),
            scalar_param(ffi, keepalive, b"b", 0.03),
            scalar_param(ffi, keepalive, b"sigma", 0.01),
            scalar_param(ffi, keepalive, b"r0", 0.02),
        ],
    )
    model = lib.engine_abi_create_model(ffi.new("char[]", b"HullWhite1F"), hw_params, len(hw_params))
    if model == ffi.NULL:
        raise RuntimeError(f"engine_abi_create_model(HullWhite1F): {last_error(ffi, lib)}")

    irs_params = ffi.new(
        "EngineParam[]",
        [
            scalar_param(ffi, keepalive, b"notional", 1_000_000.0),
            vector_param(ffi, keepalive, b"payment_times", [1.0, 2.0, 3.0, 4.0, 5.0]),
            vector_param(ffi, keepalive, b"accruals", [1.0] * 5),
        ],
    )
    product = lib.engine_abi_create_product(ffi.new("char[]", b"IRSwap"), irs_params, len(irs_params))
    if product == ffi.NULL:
        lib.engine_abi_free_model(model)
        raise RuntimeError(f"engine_abi_create_product(IRSwap): {last_error(ffi, lib)}")

    # --- ExposureProfile / UnilateralCVA: mismo caso/semillas que
    # clients/excel/README.md ("Verificacion manual") -- si estos numeros no coinciden, algo
    # se rompio en la traduccion C ABI <-> engine::Registries/IMeasure. ----------------------
    profile_measure = lib.engine_abi_create_measure(ffi.new("char[]", b"ExposureProfile"))
    assert profile_measure != ffi.NULL, f"engine_abi_create_measure(ExposureProfile): {last_error(ffi, lib)}"
    profile_params = ffi.new(
        "EngineParam[]",
        [
            vector_param(ffi, keepalive, b"monitoring_times", [0.0, 1.0, 2.0]),
            scalar_param(ffi, keepalive, b"n_paths", 5000.0),
            scalar_param(ffi, keepalive, b"seed", 7.0),
        ],
    )
    profile_result = ffi.new("EngineMeasureResult *")
    rc = lib.engine_abi_evaluate(profile_measure, model, product, profile_params, len(profile_params), profile_result)
    assert rc == 0, f"engine_abi_evaluate(ExposureProfile): {last_error(ffi, lib)}"
    ee = [profile_result.primary[i] for i in range(profile_result.len)]
    print(f"ExposureProfile EE = {ee}  (esperado [0.0, 12862.62, 13673.53])")
    lib.engine_abi_free_measure_result(profile_result)
    lib.engine_abi_free_measure(profile_measure)

    cva_measure = lib.engine_abi_create_measure(ffi.new("char[]", b"UnilateralCVA"))
    assert cva_measure != ffi.NULL, f"engine_abi_create_measure(UnilateralCVA): {last_error(ffi, lib)}"
    cva_params = ffi.new(
        "EngineParam[]",
        [
            vector_param(ffi, keepalive, b"monitoring_times", [0.0, 1.0, 2.0, 3.0]),
            scalar_param(ffi, keepalive, b"n_paths", 5000.0),
            scalar_param(ffi, keepalive, b"seed", 13.0),
            scalar_param(ffi, keepalive, b"hazard_rate", 0.02),
            scalar_param(ffi, keepalive, b"recovery_rate", 0.4),
        ],
    )
    cva_result = ffi.new("EngineMeasureResult *")
    rc = lib.engine_abi_evaluate(cva_measure, model, product, cva_params, len(cva_params), cva_result)
    assert rc == 0, f"engine_abi_evaluate(UnilateralCVA): {last_error(ffi, lib)}"
    print(f"UnilateralCVA = {cva_result.scalar}  (esperado 426.7618244093184)")
    lib.engine_abi_free_measure_result(cva_result)
    lib.engine_abi_free_measure(cva_measure)

    lib.engine_abi_free_product(product)
    lib.engine_abi_free_model(model)

    # --- Backend de computo (PLAN.md §7.12), misma ABI --------------------------------------
    backend = get_compute_backend(ffi, lib)
    gpu_available = lib.engine_abi_is_gpu_backend_available() != 0
    print(f"backend: {backend}  (gpu disponible: {'si' if gpu_available else 'no'})")
    assert lib.engine_abi_set_compute_backend(ffi.new("char[]", b"cpu")) == 1, '"cpu" siempre debe aceptarse'

    # --- Manejo de errores (PLAN.md §5.5): nunca lanza/aborta al otro lado de la ABI, un
    # nombre desconocido devuelve NULL -- se distingue de un puntero valido comparando con
    # ffi.NULL (cffi no trata los punteros nulos como "falsy" en un bool() normal).
    unknown = lib.engine_abi_create_model(ffi.new("char[]", b"NoExiste"), ffi.NULL, 0)
    assert unknown == ffi.NULL, "se esperaba NULL al pedir un modelo inexistente"
    print(f"error esperado al pedir un modelo inexistente: {last_error(ffi, lib)}")

    print("OK: ejemplo de Python (cffi) sobre engine/abi.h completado.")


if __name__ == "__main__":
    main()
