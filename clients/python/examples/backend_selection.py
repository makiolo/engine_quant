"""Ejemplo: seleccionar el backend de computo (CPU/GPU) desde Python (PLAN.md §7.12).

`engine` ya expone un gestor de contexto nativo listo para usar (implementado en C++/nanobind,
no en Python -- ver `clients/python/src/engine_py_ext.cpp`, `BackendScope`):

    with engine.backend("gpu"):
        resultado = medida.evaluate(modelo, producto, params)  # corre en GPU
    # aqui ya se ha restaurado el backend anterior, incluso si el bloque lanzo una excepcion

Ese `with engine.backend(...)` es, para quien solo quiere usarlo, todo lo que hace falta: no
requiere `contextlib`. Este fichero muestra ademas, con fines educativos, como se construiria
el mismo patron a mano en Python puro con `contextlib.contextmanager` por encima de las dos
funciones de mas bajo nivel que tambien expone el modulo (`set_compute_backend`/
`get_compute_backend`) -- util si algun dia se quisiera anadir logica extra alrededor (logging,
metricas, o combinarlo con otro contexto) sin tocar el binding nativo.

Inspiracion: `decimal.localcontext()` (stdlib) y `torch.device(...)` (PyTorch) tratan la
precision/el dispositivo de computo por defecto como un *contexto ambiente* que se puede acotar
a un bloque `with`, en vez de un parametro que hay que colar en cada llamada -- el mismo
problema que resuelve aqui `engine.backend()`.
"""

import contextlib
import sys
import time
from pathlib import Path

# El modulo compilado (engine.pyd) vive en el directorio de build de CMake (PLAN.md §7.1),
# igual que en clients/python/tests/test_*.py.
if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "build" / "clients" / "python"))

import engine  # noqa: E402


# --- Equivalente "hecho a mano" con contextlib, solo con fines ilustrativos --------------
#
# `engine.backend(...)` de arriba ya hace exactamente esto (y ademas valida el nombre y da un
# mensaje de error mas rico) -- no hace falta definir esta funcion para usar el motor, se deja
# aqui como referencia de "como se construiria este patron sobre las funciones de mas bajo
# nivel" para quien prefiera no depender del gestor de contexto nativo.
@contextlib.contextmanager
def backend(name: str):
    previous = engine.get_compute_backend()
    if not engine.set_compute_backend(name):
        available = "si" if engine.is_gpu_backend_available() else "no"
        raise ValueError(f"Backend de computo no disponible: '{name}' (gpu disponible: {available})")
    try:
        yield name.lower()
    finally:
        engine.set_compute_backend(previous)


def _par_irs_5y_and_hull_white():
    eng = engine.Engine()
    model = eng.create_model("HullWhite1F", {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02})
    product = eng.create_product(
        "IRSwap",
        {
            "notional": 1_000_000.0,
            "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
            "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
        },
    )
    measure = eng.create_measure("ExposureProfile")
    return model, product, measure


def main():
    print("backend por defecto:", engine.get_compute_backend())

    model, product, measure = _par_irs_5y_and_hull_white()
    params = {"monitoring_times": [0.0, 1.0, 2.0, 3.0, 4.0], "n_paths": 200_000.0, "seed": 7.0}

    # 1) Gestor de contexto nativo (recomendado): acota el backend al bloque `with`.
    start = time.perf_counter()
    with engine.backend("cpu"):
        profile = measure.evaluate(model, product, params)
    elapsed = time.perf_counter() - start
    print(f"CPU: EE(t_max)={profile.primary[-1]:.2f}  ({elapsed * 1000:.1f} ms)")

    # 2) Igual que arriba pero con GPU, si este build la compilo (PLAN.md §7.11: feature `gpu`
    # de engine-core, opcional -- no compilada por defecto). En un build CPU-only, pedirla
    # lanza ValueError en vez de calcular en silencio sobre CPU sin avisar.
    if engine.is_gpu_backend_available():
        start = time.perf_counter()
        with engine.backend("gpu"):
            profile = measure.evaluate(model, product, params)
        elapsed = time.perf_counter() - start
        print(f"GPU: EE(t_max)={profile.primary[-1]:.2f}  ({elapsed * 1000:.1f} ms)")
    else:
        print("GPU no compilada en este build (ver PLAN.md §7.11: recompilar con -DENGINE_QUANT_ENABLE_GPU=ON).")

    # 3) La version contextlib de arriba se comporta igual (mismo estado global por debajo).
    with backend("cpu") as active:
        print("dentro del backend() de contextlib:", active)

    print("backend tras salir de todos los bloques:", engine.get_compute_backend())


if __name__ == "__main__":
    main()
