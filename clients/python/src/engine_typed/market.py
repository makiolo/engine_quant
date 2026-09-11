"""`Market` tipado (PLAN_REAPI.md §6 Fase 2): replica en Python la validación que ya hace
`engine::Curve`/`MarketSnapshot` en C++ (pillars estrictamente creciente, mismo tamaño que
zero_rates) -- redundante con el constructor tipado que `MarketSnapshot` ya tiene, pero da
JSON schema/validación `pydantic` consistente con el resto de `engine_typed`. La validación de
aquí no sustituye la de C++: `engine.MarketSnapshot(**market.to_params())` la repite (mismos
invariantes, mismos mensajes de error en la práctica).
"""

from typing import List

from pydantic import BaseModel, ConfigDict, model_validator


class Market(BaseModel):
    """Curva de mercado observada -- o fabricada -- en un instante dado
    (`engine::MarketSnapshot`, PLAN.md §7.14/§7.15/§7.20)."""

    model_config = ConfigDict(frozen=True)

    pillars: List[float]
    zero_rates: List[float]
    hazard_rate: float = 0.0
    recovery_rate: float = 0.0

    @model_validator(mode="after")
    def _check_curve_invariants(self) -> "Market":
        if len(self.pillars) != len(self.zero_rates):
            raise ValueError("Market: pillars y zero_rates deben tener el mismo tamaño")
        if not self.pillars:
            raise ValueError("Market: necesita al menos un pillar")
        for previous, current in zip(self.pillars, self.pillars[1:]):
            if current <= previous:
                raise ValueError("Market: pillars debe ser estrictamente creciente")
        return self

    def to_params(self) -> dict:
        """Traduce este spec al `Params`/dict que espera `engine.MarketSnapshot(**...)`."""
        return {
            "pillars": self.pillars,
            "zero_rates": self.zero_rates,
            "hazard_rate": self.hazard_rate,
            "recovery_rate": self.recovery_rate,
        }
