"""`Model` tipados (PLAN_REAPI.md §6 Fase 2): mismo patrón `BaseModel` + `.to_params()` +
`.model_type` que `TradeSpec` (`engine_typed.trade`). Todos los campos son requeridos, igual
que en C++ (`HullWhite1FModel`/`HullWhite2FModel`, `get_double` sin default) -- no hay
sentinel de "por defecto" que replicar aquí.
"""

from typing import ClassVar, List

from pydantic import BaseModel, ConfigDict, model_validator


class ModelSpec(BaseModel):
    model_config = ConfigDict(frozen=True)

    model_type: ClassVar[str]

    def to_params(self) -> dict:
        """Traduce este spec al `Params`/dict que espera `Engine.create_model(model_type, ...)`."""
        raise NotImplementedError


class HullWhite1F(ModelSpec):
    """Hull-White de 1 factor (`engine::HullWhite1FModel`, PLAN.md §7.5)."""

    model_type: ClassVar[str] = "HullWhite1F"

    a: float
    b: float
    sigma: float
    r0: float

    def to_params(self) -> dict:
        return {"a": self.a, "b": self.b, "sigma": self.sigma, "r0": self.r0}


class HullWhite2F(ModelSpec):
    """Hull-White de 2 factores / G2++ (`engine::HullWhite2FModel`, PLAN.md §7.16)."""

    model_type: ClassVar[str] = "HullWhite2F"

    a: float
    b: float
    sigma: float
    eta: float
    rho: float
    r0: float

    def to_params(self) -> dict:
        return {
            "a": self.a,
            "b": self.b,
            "sigma": self.sigma,
            "eta": self.eta,
            "rho": self.rho,
            "r0": self.r0,
        }


def _cholesky_lower(matrix: List[List[float]]) -> List[List[float]]:
    """Descomposicion de Cholesky en Python puro -- MISMO algoritmo que
    `payoff::hedge::cholesky_decompose` en Rust (`rust/crates/engine-core/src/payoff/hedge.rs`),
    reutilizado aqui solo para VALIDAR que `correlation` es semidefinida positiva
    (PLAN_IMPROVE_NOTEBOOK.md Fase 3, último punto: "validación de la matriz de correlación en
    Python ... simetría y semidefinida-positiva (PSD)"). Sin depender de `numpy` (`engine_typed`
    no lo importa hoy en ningún otro sitio) solo para esta comprobación de tamaño de matriz
    pequeño (unos pocos activos como mucho). `ValueError` con un pivote no positivo en vez de
    devolver `NaN`/`None` en silencio -- misma disciplina que el `Err` de Rust.
    """
    n = len(matrix)
    lower = [[0.0] * n for _ in range(n)]
    for j in range(n):
        total = matrix[j][j] - sum(lower[j][k] ** 2 for k in range(j))
        if total <= 0.0:
            raise ValueError(
                f"GbmBasket: 'correlation' no es semidefinida positiva (Cholesky encontró un pivote no "
                f"positivo en la fila {j}) -- revisa que sea una matriz de correlación válida"
            )
        lower[j][j] = total**0.5
        for i in range(j + 1, n):
            s = matrix[i][j] - sum(lower[i][k] * lower[j][k] for k in range(j))
            lower[i][j] = s / lower[j][j]
    return lower


class GbmBasket(ModelSpec):
    """Movimiento geométrico browniano MULTI-ACTIVO correlacionado bajo Q
    (`engine::GbmBasketModel`, PLAN_IMPROVE_NOTEBOOK.md Fase 3): generaliza `Gbm`/`GBM` (un único
    `observable`) a `n_assets` observables que comparten el mismo browniano bajo una matriz de
    correlación instantánea constante -- primer modelo del motor para baskets/spreads/worst-of/
    best-of/quanto (PLAN_IMPROVE_NOTEBOOK.md §1, fricción 3). `to_params()` traduce
    `observables: list[str]` a un único string delimitado por comas (decisión de diseño de esta
    fase, ver el doc-comment de `engine::GbmBasketModel` en `cpp/engine/include/engine/model.hpp`
    y `dict_to_params` en `clients/python/src/engine_py_ext.cpp`: `ParamValue` no gana un
    variante `vector<string>` solo para esto) y `correlation: list[list[float]]` a un único
    `vector<double>` aplanado FILA A FILA -- ninguna capa reordena.
    """

    model_type: ClassVar[str] = "GbmBasket"

    observables: List[str]
    s0: List[float]
    r: List[float]
    q: List[float]
    sigma: List[float]
    # n x n, fila a fila -- correlation[i][j] es la correlación entre el activo i y el activo j.
    correlation: List[List[float]]

    @model_validator(mode="after")
    def _check_basket_invariants(self) -> "GbmBasket":
        n = len(self.observables)
        if n == 0:
            raise ValueError("GbmBasket: 'observables' no puede estar vacío")
        if not (len(self.s0) == len(self.r) == len(self.q) == len(self.sigma) == n):
            raise ValueError(
                f"GbmBasket: 'observables' declara {n} activos, pero s0/r/q/sigma no tienen todos esa "
                f"misma longitud (s0={len(self.s0)}, r={len(self.r)}, q={len(self.q)}, sigma={len(self.sigma)})"
            )
        if len(self.correlation) != n or any(len(row) != n for row in self.correlation):
            raise ValueError(f"GbmBasket: 'correlation' debe ser una matriz {n}x{n} (uno por activo declarado)")
        for i in range(n):
            if abs(self.correlation[i][i] - 1.0) > 1e-9:
                raise ValueError(
                    "GbmBasket: la diagonal de 'correlation' debe ser 1.0 (correlación de un activo consigo "
                    "mismo)"
                )
            for j in range(i + 1, n):
                if abs(self.correlation[i][j] - self.correlation[j][i]) > 1e-9:
                    raise ValueError(
                        f"GbmBasket: 'correlation' debe ser simétrica (correlation[{i}][{j}] != "
                        f"correlation[{j}][{i}])"
                    )
        # PSD: intenta la MISMA factorización de Cholesky que la simulación en Rust va a aplicar
        # -- si falla aquí, falla en Python con un mensaje claro en vez de cruzar la frontera y
        # fallar del lado de Rust con un error menos amigable.
        _cholesky_lower(self.correlation)
        return self

    def to_params(self) -> dict:
        return {
            "observables": self.observables,
            "s0": self.s0,
            "r": self.r,
            "q": self.q,
            "sigma": self.sigma,
            "correlation": [value for row in self.correlation for value in row],
        }
