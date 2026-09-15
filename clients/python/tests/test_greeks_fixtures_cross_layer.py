"""PLAN_GREEKS.md §9.4 (criterio de aceptacion cruzado de la Fase 9): el MISMO fixture JSON
(docs/schema/engine.payoff/examples/call.json, reutilizado de PLAN_PRODUCTS.md) debe producir
el mismo conjunto de risk_factor, el mismo valor por factor y el mismo `skipped` en nanobind
(engine.Engine.all_greeks, esta misma capa Python) y en la C ABI cruda (engine_abi.dll via
ctypes, la misma superficie que consumiria Julia/.NET/Go) -- la cobertura C++ nucleo vs C ABI
vive en cpp/engine/tests/test_greeks.cpp::GreeksFase9Test, mismo patron que
test_payoff_fixtures_cross_layer.cpp/.py ya establecieron para explain()/hash. Juntos, los dos
tests satisfacen "ejercitado desde C++, Python y la sonda C ABI en un unico test de
integracion" sin reimplementar la infraestructura ctypes de test_payoff_fixtures_cross_layer.py.
"""

import ctypes
import math
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parents[3]
_EXAMPLES_DIR = _REPO_ROOT / "docs" / "schema" / "engine.payoff" / "examples"


class EngineParam(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("kind", ctypes.c_int),
        ("scalar", ctypes.c_double),
        ("values", ctypes.POINTER(ctypes.c_double)),
        ("count", ctypes.c_size_t),
        ("string_value", ctypes.c_char_p),
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


class EngineGreekResultEntry(ctypes.Structure):
    _fields_ = [
        ("risk_factor", ctypes.c_char_p),
        ("method_used", ctypes.c_char_p),
        ("measure", ctypes.c_char_p),
        ("result", EngineMeasureResult),
        ("has_std_error", ctypes.c_int),
        ("std_error", ctypes.c_double),
        ("has_bump", ctypes.c_int),
        ("bump_used", ctypes.c_double),
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
    _fields_ = [("backend", ctypes.c_char_p), ("precision", ctypes.c_char_p)]


ENGINE_PARAM_DOUBLE = 0
ENGINE_PARAM_STRING = 3

_N_PATHS = 50_000
_SEED = 7


def _load_abi(dll_path: str):
    lib = ctypes.CDLL(dll_path)
    lib.engine_abi_create_product.argtypes = [ctypes.c_char_p, ctypes.POINTER(EngineParam), ctypes.c_size_t]
    lib.engine_abi_create_product.restype = ctypes.c_void_p
    lib.engine_abi_create_model.argtypes = [ctypes.c_char_p, ctypes.POINTER(EngineParam), ctypes.c_size_t]
    lib.engine_abi_create_model.restype = ctypes.c_void_p
    lib.engine_abi_free_product.argtypes = [ctypes.c_void_p]
    lib.engine_abi_free_product.restype = None
    lib.engine_abi_free_model.argtypes = [ctypes.c_void_p]
    lib.engine_abi_free_model.restype = None
    lib.engine_abi_all_greeks.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(EngineParam), ctypes.c_size_t, ctypes.c_void_p,
        ctypes.POINTER(EngineMarketSnapshot), ctypes.POINTER(EnginePricingContext), ctypes.POINTER(EngineExecutionContext),
        ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.POINTER(EngineGreekResultEntry)), ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_char_p)), ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.engine_abi_all_greeks.restype = ctypes.c_int
    lib.engine_abi_free_greeks_report.argtypes = [
        ctypes.POINTER(EngineGreekResultEntry), ctypes.c_size_t, ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t,
    ]
    lib.engine_abi_free_greeks_report.restype = None
    lib.engine_abi_last_error.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.engine_abi_last_error.restype = ctypes.c_size_t
    return lib


def _last_error(lib) -> str:
    length = lib.engine_abi_last_error(None, 0)
    buffer = ctypes.create_string_buffer(length + 1)
    lib.engine_abi_last_error(buffer, length + 1)
    return buffer.value.decode("utf-8")


def _abi_all_greeks(lib, spec: str):
    spec_bytes = spec.encode("utf-8")
    spec_param = (EngineParam * 1)()
    spec_param[0].key = b"spec"
    spec_param[0].kind = ENGINE_PARAM_STRING
    spec_param[0].string_value = spec_bytes
    product = lib.engine_abi_create_product(b"Payoff", spec_param, 1)
    assert product, _last_error(lib)

    model_params = (EngineParam * 5)()
    for i, (key, value) in enumerate(
        [("s0", 100.0), ("r", 0.05), ("q", 0.0), ("sigma", 0.2)]
    ):
        model_params[i].key = key.encode("utf-8")
        model_params[i].kind = ENGINE_PARAM_DOUBLE
        model_params[i].scalar = value
    model_params[4].key = b"observable"
    model_params[4].kind = ENGINE_PARAM_STRING
    model_params[4].string_value = b"EQ.SPOT.AAPL"
    model = lib.engine_abi_create_model(b"GBM", model_params, 5)
    assert model, _last_error(lib)

    pillars = (ctypes.c_double * 1)(1.0)
    zero_rates = (ctypes.c_double * 1)(0.05)
    market = EngineMarketSnapshot(pillars, zero_rates, 1, 0.0, 0.0)
    pricing = EnginePricingContext(0.0, _N_PATHS, 1, _SEED)
    execution = EngineExecutionContext(b"cpu", b"fp64")

    greek_entries = ctypes.POINTER(EngineGreekResultEntry)()
    n_greeks = ctypes.c_size_t()
    skipped = ctypes.POINTER(ctypes.c_char_p)()
    n_skipped = ctypes.c_size_t()
    try:
        rc = lib.engine_abi_all_greeks(
            product, b"PayoffPriceQ", None, 0, model, ctypes.byref(market), ctypes.byref(pricing),
            ctypes.byref(execution), 0, 0, ctypes.byref(greek_entries), ctypes.byref(n_greeks),
            ctypes.byref(skipped), ctypes.byref(n_skipped),
        )
        assert rc == 0, _last_error(lib)

        greeks = {}
        for i in range(n_greeks.value):
            entry = greek_entries[i]
            greeks[entry.risk_factor.decode("utf-8")] = {
                "value": entry.result.scalar,
                "method_used": entry.method_used.decode("utf-8"),
                "measure": entry.measure.decode("utf-8"),
                "bump_used": entry.bump_used if entry.has_bump else None,
            }
        skipped_list = [skipped[i].decode("utf-8") for i in range(n_skipped.value)]
        return greeks, skipped_list
    finally:
        lib.engine_abi_free_greeks_report(greek_entries, n_greeks, skipped, n_skipped)
        lib.engine_abi_free_product(product)
        lib.engine_abi_free_model(model)


def test_all_greeks_matches_between_nanobind_and_c_abi_for_the_call_fixture(abi_dll_path: str):
    lib = _load_abi(abi_dll_path)
    spec = (_EXAMPLES_DIR / "call.json").read_text(encoding="utf-8")

    eng = engine.Engine()
    product = eng.create_product("Payoff", {"spec": spec})
    model = eng.create_model("GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"})
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.05])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": float(_N_PATHS), "n_steps": 1.0, "seed": float(_SEED)})
    execution = engine.ExecutionContext({"backend": "cpu"})

    report = eng.all_greeks(product, "PayoffPriceQ", model, market, pricing, execution)
    abi_greeks, abi_skipped = _abi_all_greeks(lib, spec)

    nanobind_factors = {g.risk_factor for g in report.greeks}
    assert nanobind_factors == set(abi_greeks.keys())

    for g in report.greeks:
        abi_entry = abi_greeks[g.risk_factor]
        assert math.isclose(g.value, abi_entry["value"], rel_tol=0.0, abs_tol=1e-9), g.risk_factor
        assert g.method_used == abi_entry["method_used"], g.risk_factor
        assert g.measure == abi_entry["measure"], g.risk_factor
        assert (g.bump_used is None) == (abi_entry["bump_used"] is None), g.risk_factor

    assert sorted(report.skipped) == sorted(abi_skipped)


if __name__ == "__main__":
    if len(sys.argv) <= 1:
        raise SystemExit("uso: test_greeks_fixtures_cross_layer.py <dir del .pyd> [<engine_abi.dll>]")
    dll = sys.argv[2] if len(sys.argv) > 2 else str(_REPO_ROOT / "build" / "cpp" / "engine" / "engine_abi.dll")
    test_all_greeks_matches_between_nanobind_and_c_abi_for_the_call_fixture(dll)
    print("OK: Engine.all_greeks (nanobind) coincide con engine_abi_all_greeks (ctypes) para call.json")
