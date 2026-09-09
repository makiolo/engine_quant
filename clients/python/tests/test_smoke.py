"""Fase 0: valida que el pipeline Rust -> cxx -> C++ -> nanobind -> Python funciona de punta
a punta (PLAN.md §7.1). No hay lógica de negocio todavía; eso llega en Fase 1-3."""

import sys
from pathlib import Path

# El módulo compilado (engine.pyd) vive en el directorio de build de CMake;
# la localización definitiva del artefacto se resuelve en Fase 3 (empaquetado del cliente Python).
if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def test_ping():
    assert engine.ping() == 42.0


if __name__ == "__main__":
    test_ping()
    print("OK: engine.ping() == 42.0")
