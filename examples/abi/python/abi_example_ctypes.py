"""Ejemplo de Python consumiendo engine/abi.h (PLAN.md Fase 6, §5.5/§7.13) via `ctypes`
(solo libreria estandar), SIN pasar por el binding nanobind de clients/python (ese es un
`.pyd` compilado para una version exacta de CPython -- ver clients/python/CMakeLists.txt --
mientras que esta C ABI es un `.dll`/`.so` plano que ctypes puede cargar en tiempo de
ejecucion sin compilar nada especifico de Python). Es la demostracion mas directa de lo que
"universal" quiere decir en PLAN.md §5.5: cualquier version de Python de los ultimos 20 anos
con ctypes en la libreria estandar puede hablar con el motor.

Requiere haber compilado antes el arbol CMake de este repo (ver examples/abi/README.md): por
defecto busca engine_abi.dll/.so en <repo>/build/cpp/engine, o en la ruta que indique la
variable de entorno ENGINE_ABI_LIB_DIR/ENGINE_ABI_LIB_PATH.

Ver tambien abi_example_cffi.py: mismo recorrido, con la libreria `cffi` en vez de `ctypes`.

    python examples/abi/python/abi_example_ctypes.py
"""

import ctypes
import os
import sys
from pathlib import Path


class EngineParamKind:
    DOUBLE = 0
    VECTOR = 1
    BOOL = 2


class EngineParam(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("kind", ctypes.c_int),
        ("scalar", ctypes.c_double),
        ("values", ctypes.POINTER(ctypes.c_double)),
        ("count", ctypes.c_size_t),
    ]


class EngineMeasureResult(ctypes.Structure):
    _fields_ = [
        ("times", ctypes.POINTER(ctypes.c_double)),
        ("primary", ctypes.POINTER(ctypes.c_double)),
        ("secondary", ctypes.POINTER(ctypes.c_double)),
        ("len", ctypes.c_size_t),
        ("has_scalar", ctypes.c_int),
        ("scalar", ctypes.c_double),
    ]


def _default_lib_dir() -> Path:
    # examples/abi/python/abi_example.py -> <repo>/build/cpp/engine
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


def load_engine_abi() -> ctypes.CDLL:
    path = _library_path()
    if not path.exists():
        raise FileNotFoundError(
            f"No se encontro {path}. Compila el arbol CMake de este repo primero "
            "(ver examples/abi/README.md), o define ENGINE_ABI_LIB_PATH/ENGINE_ABI_LIB_DIR."
        )
    lib = ctypes.CDLL(str(path))

    lib.engine_abi_version.restype = ctypes.c_int

    lib.engine_abi_list_models.argtypes = [ctypes.POINTER(ctypes.POINTER(ctypes.c_char_p))]
    lib.engine_abi_list_models.restype = ctypes.c_size_t
    lib.engine_abi_free_string_list.argtypes = [ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t]

    lib.engine_abi_create_model.argtypes = [ctypes.c_char_p, ctypes.POINTER(EngineParam), ctypes.c_size_t]
    lib.engine_abi_create_model.restype = ctypes.c_void_p
    lib.engine_abi_create_product.argtypes = [ctypes.c_char_p, ctypes.POINTER(EngineParam), ctypes.c_size_t]
    lib.engine_abi_create_product.restype = ctypes.c_void_p
    lib.engine_abi_create_measure.argtypes = [ctypes.c_char_p]
    lib.engine_abi_create_measure.restype = ctypes.c_void_p
    lib.engine_abi_free_model.argtypes = [ctypes.c_void_p]
    lib.engine_abi_free_product.argtypes = [ctypes.c_void_p]
    lib.engine_abi_free_measure.argtypes = [ctypes.c_void_p]

    lib.engine_abi_evaluate.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.POINTER(EngineParam),
        ctypes.c_size_t,
        ctypes.POINTER(EngineMeasureResult),
    ]
    lib.engine_abi_evaluate.restype = ctypes.c_int
    lib.engine_abi_free_measure_result.argtypes = [ctypes.POINTER(EngineMeasureResult)]

    lib.engine_abi_set_compute_backend.argtypes = [ctypes.c_char_p]
    lib.engine_abi_set_compute_backend.restype = ctypes.c_int
    lib.engine_abi_get_compute_backend.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.engine_abi_get_compute_backend.restype = ctypes.c_size_t
    lib.engine_abi_is_gpu_backend_available.restype = ctypes.c_int

    lib.engine_abi_last_error.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.engine_abi_last_error.restype = ctypes.c_size_t

    return lib


def last_error(lib: ctypes.CDLL) -> str:
    buffer = ctypes.create_string_buffer(512)
    length = lib.engine_abi_last_error(buffer, len(buffer))
    return buffer.raw[:length].decode("utf-8", errors="replace")


def get_compute_backend(lib: ctypes.CDLL) -> str:
    buffer = ctypes.create_string_buffer(16)
    length = lib.engine_abi_get_compute_backend(buffer, len(buffer))
    return buffer.raw[:length].decode("ascii")


def scalar_param(key: bytes, value: float) -> EngineParam:
    return EngineParam(key=key, kind=EngineParamKind.DOUBLE, scalar=value, values=None, count=0)


def vector_param(key: bytes, values):
    array = (ctypes.c_double * len(values))(*values)
    # `array` debe seguir viva mientras se use el EngineParam devuelto -- el llamador es
    # responsable de mantener una referencia (ver el uso mas abajo: se guarda en una lista).
    return EngineParam(key=key, kind=EngineParamKind.VECTOR, scalar=0.0, values=array, count=len(values)), array


def main() -> None:
    lib = load_engine_abi()
    print("engine_abi_version() =", lib.engine_abi_version())

    # --- list_models: confirma que HullWhite1F esta registrado -----------------------------
    names_ptr = ctypes.POINTER(ctypes.c_char_p)()
    count = lib.engine_abi_list_models(ctypes.byref(names_ptr))
    names = [names_ptr[i].decode("utf-8") for i in range(count)]
    lib.engine_abi_free_string_list(names_ptr, count)
    assert "HullWhite1F" in names, f"HullWhite1F no aparece en engine_abi_list_models: {names}"

    # --- Caso base: IRS 5y anual a la par bajo Hull-White 1F (PLAN.md §5.2) ----------------
    hw_params = (EngineParam * 4)(
        scalar_param(b"a", 0.1), scalar_param(b"b", 0.03), scalar_param(b"sigma", 0.01), scalar_param(b"r0", 0.02)
    )
    model = lib.engine_abi_create_model(b"HullWhite1F", hw_params, len(hw_params))
    if not model:
        raise RuntimeError(f"engine_abi_create_model(HullWhite1F): {last_error(lib)}")

    payment_times_param, payment_times_arr = vector_param(b"payment_times", [1.0, 2.0, 3.0, 4.0, 5.0])
    accruals_param, accruals_arr = vector_param(b"accruals", [1.0] * 5)
    irs_params = (EngineParam * 3)(scalar_param(b"notional", 1_000_000.0), payment_times_param, accruals_param)
    product = lib.engine_abi_create_product(b"IRSwap", irs_params, len(irs_params))
    if not product:
        lib.engine_abi_free_model(model)
        raise RuntimeError(f"engine_abi_create_product(IRSwap): {last_error(lib)}")

    # --- ExposureProfile / UnilateralCVA: mismo caso/semillas que
    # clients/excel/README.md ("Verificacion manual") -- si estos numeros no coinciden, algo
    # se rompio en la traduccion C ABI <-> engine::Registries/IMeasure. ----------------------
    profile_measure = lib.engine_abi_create_measure(b"ExposureProfile")
    assert profile_measure, f"engine_abi_create_measure(ExposureProfile): {last_error(lib)}"
    profile_times_param, profile_times_arr = vector_param(b"monitoring_times", [0.0, 1.0, 2.0])
    profile_params = (EngineParam * 3)(profile_times_param, scalar_param(b"n_paths", 5000.0), scalar_param(b"seed", 7.0))
    profile_result = EngineMeasureResult()
    rc = lib.engine_abi_evaluate(profile_measure, model, product, profile_params, len(profile_params), ctypes.byref(profile_result))
    assert rc == 0, f"engine_abi_evaluate(ExposureProfile): {last_error(lib)}"
    ee = [profile_result.primary[i] for i in range(profile_result.len)]
    print(f"ExposureProfile EE = {ee}  (esperado [0.0, 12862.62, 13673.53])")
    lib.engine_abi_free_measure_result(ctypes.byref(profile_result))
    lib.engine_abi_free_measure(profile_measure)

    cva_measure = lib.engine_abi_create_measure(b"UnilateralCVA")
    assert cva_measure, f"engine_abi_create_measure(UnilateralCVA): {last_error(lib)}"
    cva_times_param, cva_times_arr = vector_param(b"monitoring_times", [0.0, 1.0, 2.0, 3.0])
    cva_params = (EngineParam * 5)(
        cva_times_param,
        scalar_param(b"n_paths", 5000.0),
        scalar_param(b"seed", 13.0),
        scalar_param(b"hazard_rate", 0.02),
        scalar_param(b"recovery_rate", 0.4),
    )
    cva_result = EngineMeasureResult()
    rc = lib.engine_abi_evaluate(cva_measure, model, product, cva_params, len(cva_params), ctypes.byref(cva_result))
    assert rc == 0, f"engine_abi_evaluate(UnilateralCVA): {last_error(lib)}"
    print(f"UnilateralCVA = {cva_result.scalar}  (esperado 426.7618244093184)")
    lib.engine_abi_free_measure_result(ctypes.byref(cva_result))
    lib.engine_abi_free_measure(cva_measure)

    lib.engine_abi_free_product(product)
    lib.engine_abi_free_model(model)

    # --- Backend de computo (PLAN.md §7.12), misma ABI --------------------------------------
    backend = get_compute_backend(lib)
    gpu_available = lib.engine_abi_is_gpu_backend_available() != 0
    print(f"backend: {backend}  (gpu disponible: {'si' if gpu_available else 'no'})")
    assert lib.engine_abi_set_compute_backend(b"cpu") == 1, '"cpu" siempre debe aceptarse'

    # --- Manejo de errores (PLAN.md §5.5): nunca lanza/aborta al otro lado de la ABI, un
    # nombre desconocido devuelve NULL -- se distingue de un puntero valido comparando con 0.
    unknown = lib.engine_abi_create_model(b"NoExiste", None, 0)
    assert not unknown, "se esperaba NULL al pedir un modelo inexistente"
    print(f"error esperado al pedir un modelo inexistente: {last_error(lib)}")

    print("OK: ejemplo de Python (ctypes) sobre engine/abi.h completado.")



if __name__ == "__main__":
    main()
