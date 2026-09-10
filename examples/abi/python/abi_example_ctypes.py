"""Ejemplo de Python consumiendo engine/abi.h (PLAN.md Fase 6, §5.5/§7.13; ENGINE.CALC en
PLAN.md §7.15) via `ctypes` (solo libreria estandar), SIN pasar por el binding nanobind de
clients/python (ese es un `.pyd` compilado para una version exacta de CPython -- ver
clients/python/CMakeLists.txt -- mientras que esta C ABI es un `.dll`/`.so` plano que ctypes
puede cargar en tiempo de ejecucion sin compilar nada especifico de Python). Es la
demostracion mas directa de lo que "universal" quiere decir en PLAN.md §5.5: cualquier version
de Python de los ultimos 20 anos con ctypes en la libreria estandar puede hablar con el motor.

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


class EngineMarketSnapshot(ctypes.Structure):
    _fields_ = [
        ("pillars", ctypes.POINTER(ctypes.c_double)),
        ("zero_rates", ctypes.POINTER(ctypes.c_double)),
        ("count", ctypes.c_size_t),
        ("hazard_rate", ctypes.c_double),
        ("recovery_rate", ctypes.c_double),
    ]


class EnginePricingContext(ctypes.Structure):
    _fields_ = [
        ("pricing_date", ctypes.c_double),
        ("n_paths", ctypes.c_uint64),
        ("n_steps", ctypes.c_uint64),
        ("seed", ctypes.c_uint64),
    ]


class EngineExecutionContext(ctypes.Structure):
    _fields_ = [
        ("backend", ctypes.c_char_p),
        ("precision", ctypes.c_char_p),
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


class EngineCalcResultEntry(ctypes.Structure):
    _fields_ = [
        ("measure_name", ctypes.c_char_p),
        ("result", EngineMeasureResult),
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
    lib.engine_abi_free_model.argtypes = [ctypes.c_void_p]
    lib.engine_abi_free_product.argtypes = [ctypes.c_void_p]

    lib.engine_abi_calc.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.POINTER(EngineMarketSnapshot),
        ctypes.POINTER(EnginePricingContext),
        ctypes.POINTER(EngineExecutionContext),
        ctypes.POINTER(ctypes.POINTER(EngineCalcResultEntry)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.engine_abi_calc.restype = ctypes.c_int
    lib.engine_abi_free_calc_results.argtypes = [ctypes.POINTER(EngineCalcResultEntry), ctypes.c_size_t]

    lib.engine_abi_is_gpu_backend_available.restype = ctypes.c_int

    lib.engine_abi_last_error.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.engine_abi_last_error.restype = ctypes.c_size_t

    return lib


def last_error(lib: ctypes.CDLL) -> str:
    buffer = ctypes.create_string_buffer(512)
    length = lib.engine_abi_last_error(buffer, len(buffer))
    return buffer.raw[:length].decode("utf-8", errors="replace")


def scalar_param(key: bytes, value: float) -> EngineParam:
    return EngineParam(key=key, kind=EngineParamKind.DOUBLE, scalar=value, values=None, count=0)


def vector_param(key: bytes, values):
    array = (ctypes.c_double * len(values))(*values)
    # `array` debe seguir viva mientras se use el EngineParam devuelto -- el llamador es
    # responsable de mantener una referencia (ver el uso mas abajo: se guarda en una lista).
    return EngineParam(key=key, kind=EngineParamKind.VECTOR, scalar=0.0, values=array, count=len(values)), array


def find_measure(entries, count: int, name: str) -> EngineMeasureResult:
    for i in range(count):
        if entries[i].measure_name.decode("utf-8") == name:
            return entries[i].result
    raise KeyError(f"no se pidio la medida '{name}'")


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

    # --- ENGINE.CALC (PLAN.md §7.15): mismo caso base que cpp/engine/tests/test_registry.cpp
    # (Registry.UnilateralCvaMatchesGoldenValue/ExposureProfileMatchesGoldenValue), pero aqui
    # basta con invariantes cualitativos -- este ejemplo verifica el mecanismo de la ABI, no
    # vuelve a fijar el numero exacto. --------------------------------------------------------
    pillars = (ctypes.c_double * 1)(1.0)
    zero_rates = (ctypes.c_double * 1)(0.02)
    market = EngineMarketSnapshot(pillars=pillars, zero_rates=zero_rates, count=1, hazard_rate=0.02, recovery_rate=0.4)
    pricing = EnginePricingContext(pricing_date=0.0, n_paths=5000, n_steps=208, seed=7)
    execution = EngineExecutionContext(backend=b"cpu", precision=b"FP64")

    measure_names = (ctypes.c_char_p * 5)(b"PV", b"DV01", b"ExpectedExposure", b"PFE95", b"UnilateralCVA")
    entries_ptr = ctypes.POINTER(EngineCalcResultEntry)()
    entry_count = ctypes.c_size_t()
    rc = lib.engine_abi_calc(
        product, measure_names, len(measure_names), model, ctypes.byref(market), ctypes.byref(pricing),
        ctypes.byref(execution), ctypes.byref(entries_ptr), ctypes.byref(entry_count)
    )
    assert rc == 0, f"engine_abi_calc: {last_error(lib)}"

    pv = find_measure(entries_ptr, entry_count.value, "PV")
    dv01 = find_measure(entries_ptr, entry_count.value, "DV01")
    ee = find_measure(entries_ptr, entry_count.value, "ExpectedExposure")
    pfe = find_measure(entries_ptr, entry_count.value, "PFE95")
    cva = find_measure(entries_ptr, entry_count.value, "UnilateralCVA")

    print(f"PV            = {pv.scalar}  (swap a la par: ~0)")
    print(f"DV01          = {dv01.scalar}  (swap pagador: > 0)")
    print(f"UnilateralCVA = {cva.scalar}  (> 0 con hazard_rate > 0)")
    for i in range(ee.len):
        print(f"  t={ee.times[i]}: EE={ee.primary[i]}  PFE95={pfe.primary[i]}")

    assert abs(pv.scalar) < 1e-6, "PV de un swap a la par deberia ser ~0"
    assert dv01.scalar > 0.0 and cva.scalar > 0.0, "se esperaba DV01 > 0 y UnilateralCVA > 0"
    for i in range(ee.len):
        assert ee.primary[i] >= 0.0 and pfe.primary[i] >= ee.primary[i], "se esperaba PFE95 >= ExpectedExposure >= 0"

    lib.engine_abi_free_calc_results(entries_ptr, entry_count)

    gpu_available = lib.engine_abi_is_gpu_backend_available() != 0
    print(f"gpu disponible: {'si' if gpu_available else 'no'}")

    # --- engine_abi_calc rechaza un nombre de medida desconocido (PLAN.md §7.15): el error
    # queda en engine_abi_last_error(), nunca lanza/aborta a traves de esta frontera C. --------
    bad_names = (ctypes.c_char_p * 1)(b"NoExiste")
    bad_entries_ptr = ctypes.POINTER(EngineCalcResultEntry)()
    bad_count = ctypes.c_size_t()
    rc = lib.engine_abi_calc(
        product, bad_names, 1, model, ctypes.byref(market), ctypes.byref(pricing), ctypes.byref(execution),
        ctypes.byref(bad_entries_ptr), ctypes.byref(bad_count)
    )
    assert rc != 0, "se esperaba error con un nombre de medida desconocido"
    print(f"error esperado al pedir una medida inexistente: {last_error(lib)}")

    lib.engine_abi_free_product(product)
    lib.engine_abi_free_model(model)

    # --- Manejo de errores (PLAN.md §5.5): nunca lanza/aborta al otro lado de la ABI, un
    # nombre desconocido devuelve NULL -- se distingue de un puntero valido comparando con 0.
    unknown = lib.engine_abi_create_model(b"NoExiste", None, 0)
    assert not unknown, "se esperaba NULL al pedir un modelo inexistente"
    print(f"error esperado al pedir un modelo inexistente: {last_error(lib)}")

    print("OK: ejemplo de Python (ctypes) sobre engine/abi.h (ENGINE.CALC) completado.")


if __name__ == "__main__":
    main()
