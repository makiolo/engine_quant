"""`Model` tipados (PLAN_REAPI.md §6 Fase 2): mismo patrón `BaseModel` + `.to_params()` +
`.model_type` que `TradeSpec` (`engine_typed.trade`). Todos los campos son requeridos, igual
que en C++ (`HullWhite1FModel`/`HullWhite2FModel`, `get_double` sin default) -- no hay
sentinel de "por defecto" que replicar aquí.
"""

from typing import ClassVar

from pydantic import BaseModel, ConfigDict


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
