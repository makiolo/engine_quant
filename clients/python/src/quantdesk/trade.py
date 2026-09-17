"""`TradeSpec`/`IRSwap` (PLAN_REAPI.md §6 Fase 1, propuestas 1+2).

Fachada tipada sobre el mismo `Params`/dict que ya consume `engine.Engine.create_product`
(PLAN_REAPI.md §2: mismo patrón que el segundo constructor de `MarketSnapshot`, aquí resuelto
en Python puro en vez de un segundo constructor C++). `to_params()` no valida nada que el core
no valide ya -- solo traduce.

`fixed_rate` es un campo **requerido**: omitirlo es un `ValidationError` de pydantic, no un
swap "a la par" -- para eso está `PAR`/`IRSwap.par(...)` (propuesta 2). La fachada dinámica
(dict/Excel/C ABI) no cambia: ahí "ausencia de `fixed_rate`" sigue significando PAR, tal como
documenta `IrSwapProduct` (`cpp/engine/include/engine/product.hpp`).
"""

from typing import ClassVar, List, Literal, Union

from pydantic import BaseModel, ConfigDict

PAR: Literal["PAR"] = "PAR"


class TradeSpec(BaseModel):
    model_config = ConfigDict(frozen=True)  # inmutable: construir es una decisión, no un estado mutable

    product_type: ClassVar[str]

    def to_params(self) -> dict:
        """Traduce este spec al `Params`/dict que espera `Engine.create_product(product_type, ...)`."""
        raise NotImplementedError


class IRSwap(TradeSpec):
    """IRS vanilla (mismo caso base que `engine::IrSwapProduct`, PLAN.md §5.2).

    `day_count` queda fuera deliberadamente (PLAN_REAPI.md §6 Fase 1): no existe hoy en el
    core (ni C++ ni Rust lo reciben) -- añadirlo aquí sería un campo sin efecto.
    """

    product_type: ClassVar[str] = "IRSwap"

    notional: float
    fixed_rate: Union[float, Literal["PAR"]]  # requerido: omitirlo es ValidationError, no PAR
    start: float = 0.0
    payment_times: List[float]
    accruals: List[float]

    @classmethod
    def par(
        cls,
        *,
        notional: float,
        payment_times: List[float],
        accruals: List[float],
        start: float = 0.0,
    ) -> "IRSwap":
        """Constructor con nombre para un swap "a la par" (propuesta 2): equivalente explícito
        a `IRSwap(..., fixed_rate=PAR)`."""
        return cls(notional=notional, fixed_rate=PAR, start=start, payment_times=payment_times, accruals=accruals)

    def to_params(self) -> dict:
        params: dict = {
            "notional": self.notional,
            "start": self.start,
            "payment_times": self.payment_times,
            "accruals": self.accruals,
        }
        if self.fixed_rate != PAR:
            params["fixed_rate"] = self.fixed_rate
        return params
