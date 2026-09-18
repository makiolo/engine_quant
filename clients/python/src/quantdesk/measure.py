"""Medidas tipadas (PLAN_REAPI.md §6 Fase 3, propuesta 3): `q.PV()`, `q.DV01(bump=0.0002)`,
`q.ExposureProfile()`, `q.UnilateralCVA()` en vez de strings sueltos. `.to_spec()` produce la
tupla `(nombre, params)` que ya acepta `Engine.price`/`price_batch`/`price_many`/`price_grid`
(`clients/python/src/engine_py_ext.cpp::to_measure_spec`) -- un string "pelado" sigue
funcionando igual (`eng.price(product, ["PV", "DV01"], ...)`), `.to_spec()` es solo necesario
cuando una medida lleva configuración real.

    result = eng.price(product, [q.PV().to_spec(), q.DV01(bump=0.0002).to_spec()], ...)

`.to_spec(alias=...)` (PLAN_IMPROVE_NOTEBOOK2.md Fase 3, opción (b)): produce en cambio la
tupla de 3 elementos `(nombre, params, alias)` -- necesario para pedir varias `Greek` (todas
registradas bajo el mismo `measure_name == "Greek"`, que de otro modo colisionarian en el dict
de salida) en una sola llamada a `Engine.price`/`price_grid`, cada una con su propio alias:

    result = eng.price(product, [
        greeks.delta("PayoffPriceQ", "spot").to_spec(alias="delta"),
        greeks.vega("PayoffPriceQ").to_spec(alias="vega"),
    ], model, market, pricing, execution)
    # result == {"delta": MeasureResult(...), "vega": MeasureResult(...)}

Sin `alias` (default `None`), `to_spec()` sigue devolviendo la tupla de 2 elementos de siempre
-- ningun consumidor existente de `.to_spec()` sin argumentos cambia de comportamiento."""

from typing import ClassVar, Optional, Tuple, Union

from pydantic import BaseModel, ConfigDict


class Measure(BaseModel):
    model_config = ConfigDict(frozen=True)

    measure_name: ClassVar[str]

    def to_params(self) -> dict:
        """Traduce este spec al `Params`/dict que consume la medida registrada subyacente."""
        raise NotImplementedError

    def to_spec(self, alias: Optional[str] = None) -> Union[Tuple[str, dict], Tuple[str, dict, str]]:
        """`(nombre, params)`: lo que espera `Engine.price(...)` para pasar configuración por
        medida (PLAN_REAPI.md §6 Fase 3). Con `alias` (PLAN_IMPROVE_NOTEBOOK2.md Fase 3, opción
        (b)): `(nombre, params, alias)` -- ver el doc-comment del módulo."""
        if alias is None:
            return (self.measure_name, self.to_params())
        return (self.measure_name, self.to_params(), alias)


class PV(Measure):
    """NPV determinista del trade (`engine::PresentValueMeasure`, PLAN.md §7.15). Sin
    configuración propia."""

    measure_name: ClassVar[str] = "PV"

    def to_params(self) -> dict:
        return {}


class DV01(Measure):
    """Sensibilidad del NPV a un movimiento de `bump` en la curva de mercado
    (`engine::Dv01Measure`, bump-and-reval, PLAN_REAPI.md §6 Fase 4). Primera medida con
    configuración real (PLAN_REAPI.md §6 Fase 3): `DV01(bump=0.0002).to_spec()` da un
    resultado distinto de `DV01().to_spec()` (default `bump=0.0001`, un punto básico).

    `bucketed=True` (PLAN_REAPI.md §6 Fase 5): en vez de un bump paralelo de toda la curva,
    bumpea cada pillar individualmente y devuelve un delta por pillar
    (`MeasureResult.times`=pillars, `.primary`=deltas) en vez de un escalar -- la suma de los
    deltas coincide con `DV01(bucketed=False)` sobre la misma curva/bump."""

    measure_name: ClassVar[str] = "DV01"

    bump: float = 0.0001
    bucketed: bool = False

    def to_params(self) -> dict:
        return {"bump": self.bump, "bucketed": self.bucketed}


class ExposureProfile(Measure):
    """Perfil de exposición esperada (EE) y potencial futura al 95% (PFE95) en un único
    `MeasureResult` (`.primary`=EE, `.secondary`=PFE95) -- `engine::ExposureProfileMeasure`
    por su `type_name()` real, no los alias heredados "ExpectedExposure"/"PFE95"."""

    measure_name: ClassVar[str] = "ExposureProfile"

    def to_params(self) -> dict:
        return {}


class UnilateralCVA(Measure):
    """CVA unilateral (hazard rate/recovery rate leídos de `Market`,
    `engine::UnilateralCvaMeasure`). Sin configuración propia."""

    measure_name: ClassVar[str] = "UnilateralCVA"

    def to_params(self) -> dict:
        return {}
