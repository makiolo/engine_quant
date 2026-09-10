"""Ejemplo de Python consumiendo engine/abi.h (PLAN.md Fase 6, §5.5/§7.13; ENGINE.CALC en
PLAN.md §7.15) via `cffi`, en modo ABI (`ffi.dlopen`, sin compilar una extension C -- a
diferencia del modo API de cffi, que sí compilaría un `.pyd`/`.so` propio y se parecería mas a
`clients/python`). Mismo recorrido que `abi_example_ctypes.py`: comparar ambos ficheros lado a
lado es la forma mas directa de ver la diferencia entre las dos librerias de FFI mas comunes
de Python.

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

    void engine_abi_free_model(EngineModel* model);
    void engine_abi_free_product(EngineProduct* product);

    typedef struct EngineMarketSnapshot {
        const double* pillars;
        const double* zero_rates;
        size_t count;
        double hazard_rate;
        double recovery_rate;
    } EngineMarketSnapshot;

    typedef struct EnginePricingContext {
        double pricing_date;
        uint64_t n_paths;
        uint64_t n_steps;
        uint64_t seed;
    } EnginePricingContext;

    typedef struct EngineExecutionContext {
        const char* backend;
        const char* precision;
    } EngineExecutionContext;

    typedef struct EngineMeasureResult {
        double* times;
        double* primary;
        double* secondary;
        size_t len;
        int has_scalar;
        double scalar;
    } EngineMeasureResult;

    typedef struct EngineCalcResultEntry {
        char* measure_name;
        EngineMeasureResult result;
    } EngineCalcResultEntry;

    int engine_abi_calc(
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
    void engine_abi_free_calc_results(EngineCalcResultEntry* entries, size_t count);

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


def find_measure(ffi, entries, count: int, name: str):
    for i in range(count):
        if ffi.string(entries[i].measure_name).decode("utf-8") == name:
            return entries[i].result
    raise KeyError(f"no se pidio la medida '{name}'")


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

    # --- ENGINE.CALC (PLAN.md §7.15): mismo caso base que cpp/engine/tests/test_registry.cpp
    # (Registry.UnilateralCvaMatchesGoldenValue/ExposureProfileMatchesGoldenValue), pero aqui
    # basta con invariantes cualitativos -- este ejemplo verifica el mecanismo de la ABI, no
    # vuelve a fijar el numero exacto. --------------------------------------------------------
    pillars = ffi.new("double[]", [1.0])
    zero_rates = ffi.new("double[]", [0.02])
    market = ffi.new(
        "EngineMarketSnapshot *",
        {"pillars": pillars, "zero_rates": zero_rates, "count": 1, "hazard_rate": 0.02, "recovery_rate": 0.4},
    )
    pricing = ffi.new("EnginePricingContext *", {"pricing_date": 0.0, "n_paths": 5000, "n_steps": 208, "seed": 7})
    backend_buf = ffi.new("char[]", b"cpu")
    precision_buf = ffi.new("char[]", b"FP64")
    execution = ffi.new("EngineExecutionContext *", {"backend": backend_buf, "precision": precision_buf})

    measure_name_bufs = [ffi.new("char[]", name) for name in (b"PV", b"DV01", b"ExpectedExposure", b"PFE95", b"UnilateralCVA")]
    measure_names = ffi.new("const char*[]", measure_name_bufs)

    entries_ptr = ffi.new("EngineCalcResultEntry **")
    entry_count = ffi.new("size_t *")
    rc = lib.engine_abi_calc(
        product, measure_names, len(measure_name_bufs), model, market, pricing, execution, entries_ptr, entry_count
    )
    assert rc == 0, f"engine_abi_calc: {last_error(ffi, lib)}"
    entries = entries_ptr[0]
    count = entry_count[0]

    pv = find_measure(ffi, entries, count, "PV")
    dv01 = find_measure(ffi, entries, count, "DV01")
    ee = find_measure(ffi, entries, count, "ExpectedExposure")
    pfe = find_measure(ffi, entries, count, "PFE95")
    cva = find_measure(ffi, entries, count, "UnilateralCVA")

    print(f"PV            = {pv.scalar}  (swap a la par: ~0)")
    print(f"DV01          = {dv01.scalar}  (swap pagador: > 0)")
    print(f"UnilateralCVA = {cva.scalar}  (> 0 con hazard_rate > 0)")
    for i in range(ee.len):
        print(f"  t={ee.times[i]}: EE={ee.primary[i]}  PFE95={pfe.primary[i]}")

    assert abs(pv.scalar) < 1e-6, "PV de un swap a la par deberia ser ~0"
    assert dv01.scalar > 0.0 and cva.scalar > 0.0, "se esperaba DV01 > 0 y UnilateralCVA > 0"
    for i in range(ee.len):
        assert ee.primary[i] >= 0.0 and pfe.primary[i] >= ee.primary[i], "se esperaba PFE95 >= ExpectedExposure >= 0"

    lib.engine_abi_free_calc_results(entries, count)

    gpu_available = lib.engine_abi_is_gpu_backend_available() != 0
    print(f"gpu disponible: {'si' if gpu_available else 'no'}")

    # --- engine_abi_calc rechaza un nombre de medida desconocido (PLAN.md §7.15): el error
    # queda en engine_abi_last_error(), nunca lanza/aborta a traves de esta frontera C. --------
    bad_name_buf = ffi.new("char[]", b"NoExiste")
    bad_names = ffi.new("const char*[]", [bad_name_buf])
    bad_entries_ptr = ffi.new("EngineCalcResultEntry **")
    bad_count = ffi.new("size_t *")
    rc = lib.engine_abi_calc(product, bad_names, 1, model, market, pricing, execution, bad_entries_ptr, bad_count)
    assert rc != 0, "se esperaba error con un nombre de medida desconocido"
    print(f"error esperado al pedir una medida inexistente: {last_error(ffi, lib)}")

    lib.engine_abi_free_product(product)
    lib.engine_abi_free_model(model)

    # --- Manejo de errores (PLAN.md §5.5): nunca lanza/aborta al otro lado de la ABI, un
    # nombre desconocido devuelve NULL -- se distingue de un puntero valido comparando con
    # ffi.NULL (cffi no trata los punteros nulos como "falsy" en un bool() normal).
    unknown = lib.engine_abi_create_model(ffi.new("char[]", b"NoExiste"), ffi.NULL, 0)
    assert unknown == ffi.NULL, "se esperaba NULL al pedir un modelo inexistente"
    print(f"error esperado al pedir un modelo inexistente: {last_error(ffi, lib)}")

    print("OK: ejemplo de Python (cffi) sobre engine/abi.h (ENGINE.CALC) completado.")


if __name__ == "__main__":
    main()
