"""Valida el schema JSON del motor de payoff universal (PLAN_PRODUCTS.md SS7.2, SS12 Fase 0,
ADR-P0-09): confirma que `docs/schema/engine.payoff/v1.schema.json` es un JSON Schema valido
(draft 2020-12) y que cada fixture de `docs/schema/engine.payoff/examples/` valida contra el.

No depende del motor compilado (no importa `engine`, no necesita `sys.argv[1]`): es
documentacion/contrato, verificable sin build de C++, tal como decidio Fase 0 (schema
estatico validado con `jsonschema` de Python, sin adelantar el parser real de Fase 3).
"""

import json
from pathlib import Path

import jsonschema

_SCHEMA_DIR = Path(__file__).resolve().parents[3] / "docs" / "schema" / "engine.payoff"
_SCHEMA_PATH = _SCHEMA_DIR / "v1.schema.json"
_EXAMPLES_DIR = _SCHEMA_DIR / "examples"


def _load_schema():
    with open(_SCHEMA_PATH, encoding="utf-8") as f:
        return json.load(f)


def test_schema_itself_is_a_valid_json_schema():
    schema = _load_schema()
    jsonschema.Draft202012Validator.check_schema(schema)


def test_examples_exist():
    examples = sorted(_EXAMPLES_DIR.glob("*.json"))
    expected = {"call.json", "forward.json", "swap.json", "barrier.json", "tp_sl.json"}
    found = {p.name for p in examples}
    assert expected <= found, f"faltan ejemplos: {expected - found}"


def test_all_examples_validate_against_schema():
    schema = _load_schema()
    validator = jsonschema.Draft202012Validator(schema)
    examples = sorted(_EXAMPLES_DIR.glob("*.json"))
    assert examples, f"no se encontraron ejemplos en {_EXAMPLES_DIR}"
    for example_path in examples:
        with open(example_path, encoding="utf-8") as f:
            document = json.load(f)
        errors = sorted(validator.iter_errors(document), key=lambda e: e.path)
        assert not errors, f"{example_path.name}: {[e.message for e in errors]}"


def test_unknown_field_is_rejected():
    schema = _load_schema()
    validator = jsonschema.Draft202012Validator(schema)
    document = {
        "schema": "engine.payoff/v1",
        "id": "BAD",
        "contract": {"type": "zero", "unexpected_field": 1.0},
    }
    errors = list(validator.iter_errors(document))
    assert errors, "un campo desconocido debe rechazarse (PLAN_PRODUCTS.md SS7.2)"


if __name__ == "__main__":
    test_schema_itself_is_a_valid_json_schema()
    test_examples_exist()
    test_all_examples_validate_against_schema()
    test_unknown_field_is_rejected()
    print("OK: schema engine.payoff/v1 y ejemplos (call/forward/swap/barrier/tp_sl) validan")
