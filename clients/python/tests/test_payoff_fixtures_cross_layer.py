"""PLAN_PRODUCTS.md Fase 10 (§12, Aceptacion: "los mismos JSON fixtures producen mismo hash y
resultados en las cinco capas"): confirma que cada fixture real de
docs/schema/engine.payoff/examples/*.json produce el mismo explain()/hash agregado en nanobind
(engine.Engine, esta misma capa Python) y en la C ABI cruda (engine_abi.dll via ctypes, la
misma superficie que consumiria Julia/.NET/Go) -- las capas C++ nucleo y Excel se cubren con
estos mismos fixtures en cpp/engine/tests/payoff/test_payoff_fixtures_cross_layer.cpp y
clients/excel/tests/test_xloper.cpp respectivamente. No repite la validacion de FORMA contra
el JSON Schema (eso ya lo hace test_payoff_schema.py) -- aqui basta con que ambas capas
coincidan byte a byte.
"""

import ctypes
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parents[3]
_EXAMPLES_DIR = _REPO_ROOT / "docs" / "schema" / "engine.payoff" / "examples"
_FIXTURES = ["call.json", "forward.json", "swap.json", "barrier.json", "tp_sl.json", "exercise.json", "asian.json"]


class EngineParam(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("kind", ctypes.c_int),
        ("scalar", ctypes.c_double),
        ("values", ctypes.POINTER(ctypes.c_double)),
        ("count", ctypes.c_size_t),
        ("string_value", ctypes.c_char_p),
    ]


ENGINE_PARAM_STRING = 3


def _load_abi(dll_path: str):
    lib = ctypes.CDLL(dll_path)
    lib.engine_abi_validate_payoff_spec.argtypes = [ctypes.c_char_p]
    lib.engine_abi_validate_payoff_spec.restype = ctypes.c_int
    lib.engine_abi_create_product.argtypes = [ctypes.c_char_p, ctypes.POINTER(EngineParam), ctypes.c_size_t]
    lib.engine_abi_create_product.restype = ctypes.c_void_p
    lib.engine_abi_free_product.argtypes = [ctypes.c_void_p]
    lib.engine_abi_free_product.restype = None
    lib.engine_abi_explain_product.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    lib.engine_abi_explain_product.restype = ctypes.c_size_t
    lib.engine_abi_last_error.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.engine_abi_last_error.restype = ctypes.c_size_t
    return lib


def _last_error(lib) -> str:
    length = lib.engine_abi_last_error(None, 0)
    buffer = ctypes.create_string_buffer(length + 1)
    lib.engine_abi_last_error(buffer, length + 1)
    return buffer.value.decode("utf-8")


def _abi_explain(lib, spec: str) -> str:
    spec_bytes = spec.encode("utf-8")
    assert lib.engine_abi_validate_payoff_spec(spec_bytes) == 0, _last_error(lib)

    params = (EngineParam * 1)()
    params[0].key = b"spec"
    params[0].kind = ENGINE_PARAM_STRING
    params[0].string_value = spec_bytes

    product = lib.engine_abi_create_product(b"Payoff", params, 1)
    assert product, _last_error(lib)
    try:
        length = lib.engine_abi_explain_product(product, None, 0)
        buffer = ctypes.create_string_buffer(length + 1)
        lib.engine_abi_explain_product(product, buffer, length + 1)
        return buffer.value.decode("utf-8")
    finally:
        lib.engine_abi_free_product(product)


def test_all_fixtures_match_between_nanobind_and_c_abi(abi_dll_path: str):
    lib = _load_abi(abi_dll_path)
    eng = engine.Engine()
    for name in _FIXTURES:
        spec = (_EXAMPLES_DIR / name).read_text(encoding="utf-8")

        assert engine.validate_payoff_spec(spec) == [], name
        product = eng.create_product("Payoff", {"spec": spec})
        nanobind_explain = product.explain()

        abi_explain = _abi_explain(lib, spec)
        assert nanobind_explain == abi_explain, name


if __name__ == "__main__":
    if len(sys.argv) <= 1:
        raise SystemExit("uso: test_payoff_fixtures_cross_layer.py <dir del .pyd> [<engine_abi.dll>]")
    dll = sys.argv[2] if len(sys.argv) > 2 else str(_REPO_ROOT / "build" / "cpp" / "engine" / "engine_abi.dll")
    test_all_fixtures_match_between_nanobind_and_c_abi(dll)
    print("OK: fixtures engine.payoff/v1 coinciden entre nanobind y la C ABI (ctypes)")
