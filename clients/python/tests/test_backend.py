"""Tests de la seleccion de backend de computo (PLAN.md §7.12): set_compute_backend/
get_compute_backend/is_gpu_backend_available (funciones libres, estado global de proceso) y
el gestor de contexto engine.backend() (nativo, no contextlib -- ver docstring de
engine.backend en clients/python/src/engine_py_ext.cpp). No repite la validacion numerica de
CPU vs GPU (eso ya lo hacen examples/backend_dispatch_probe.rs y examples/gpu_vs_cpu_bench.rs
en el core Rust, PLAN.md §7.11/§7.12): aqui solo se verifica el contrato de la API tal como
la ven los clientes (Python/Excel), con CpuBackend siempre disponible en el build de CI.
"""

import sys

# El modulo compilado (engine.pyd) vive en el directorio de build de CMake, igual que en
# test_smoke.py/test_registry.py (PLAN.md §7.1).
if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def test_default_backend_is_cpu():
    assert engine.get_compute_backend() == "cpu"


def test_set_compute_backend_rejects_unknown_name_without_changing_state():
    before = engine.get_compute_backend()
    assert engine.set_compute_backend("tpu") is False
    assert engine.get_compute_backend() == before


def test_set_compute_backend_cpu_is_idempotent():
    assert engine.set_compute_backend("CPU") is True  # case-insensitive
    assert engine.get_compute_backend() == "cpu"


def test_backend_context_manager_restores_previous_value_on_normal_exit():
    with engine.backend("cpu") as active:
        assert active == "cpu"
        assert engine.get_compute_backend() == "cpu"
    assert engine.get_compute_backend() == "cpu"


def test_backend_context_manager_restores_previous_value_even_if_block_raises():
    class Boom(Exception):
        pass

    try:
        with engine.backend("cpu"):
            raise Boom("da igual el motivo")
    except Boom:
        pass
    else:
        assert False, "se esperaba que Boom se propagara"

    assert engine.get_compute_backend() == "cpu"


def test_backend_context_manager_rejects_unavailable_backend_without_changing_state():
    # Este build de CI no compila la feature `gpu` (PLAN.md §7.11): pedir "gpu" debe fallar
    # con un mensaje claro, no calcular en silencio sobre CPU sin avisar.
    if engine.is_gpu_backend_available():
        return
    before = engine.get_compute_backend()
    try:
        with engine.backend("gpu"):
            assert False, "no deberia entrar al bloque si el backend no esta disponible"
    except ValueError:
        pass
    assert engine.get_compute_backend() == before


if __name__ == "__main__":
    test_default_backend_is_cpu()
    test_set_compute_backend_rejects_unknown_name_without_changing_state()
    test_set_compute_backend_cpu_is_idempotent()
    test_backend_context_manager_restores_previous_value_on_normal_exit()
    test_backend_context_manager_restores_previous_value_even_if_block_raises()
    test_backend_context_manager_rejects_unavailable_backend_without_changing_state()
    print("OK: tests de seleccion de backend (set_compute_backend/backend()) pasaron")
