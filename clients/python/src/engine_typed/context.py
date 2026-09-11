"""`PricingContext`/`ExecutionContext` tipados (PLAN_REAPI.md §6 Fase 2): hoy solo existen como
dict en Python (`engine.PricingContext({...})`/`engine.ExecutionContext({...})`) -- misma
validación que ya hacen esas clases en C++ (`cpp/engine/src/pricing_context.cpp`/
`execution_context.cpp`), aquí solo como capa tipada que produce el mismo dict de entrada.
"""

from typing import Literal

from pydantic import BaseModel, ConfigDict, field_validator


class PricingContext(BaseModel):
    """Contexto de valoración de `Engine.price` (`engine::PricingContext`, PLAN.md §7.15)."""

    model_config = ConfigDict(frozen=True)

    pricing_date: float = 0.0
    n_paths: int
    n_steps: int
    seed: int

    @field_validator("n_paths", "n_steps")
    @classmethod
    def _must_be_positive(cls, value: int) -> int:
        if value <= 0:
            raise ValueError("PricingContext: n_paths/n_steps deben ser > 0")
        return value

    def to_params(self) -> dict:
        """Traduce este spec al dict que espera `engine.PricingContext(...)`."""
        return {
            "pricing_date": self.pricing_date,
            "n_paths": float(self.n_paths),
            "n_steps": float(self.n_steps),
            "seed": float(self.seed),
        }


class ExecutionContext(BaseModel):
    """Cómo ejecutar `Engine.price` (`engine::ExecutionContext`, PLAN.md §7.15). A diferencia
    del constructor C++ (donde "backend" es requerido), aquí tiene default "auto" -- misma
    conveniencia que ya ofrece el ejemplo `price_flow.py`."""

    model_config = ConfigDict(frozen=True)

    backend: Literal["cpu", "gpu", "auto"] = "auto"
    precision: Literal["FP64", "fp64"] = "FP64"

    def to_params(self) -> dict:
        """Traduce este spec al dict que espera `engine.ExecutionContext(...)`."""
        return {"backend": self.backend, "precision": self.precision}
