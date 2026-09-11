"""Medidas tipadas (PLAN_REAPI.md §6 Fase 3, propuesta 3): `q.PV()`, `q.DV01(bump=0.0002)`,
`q.ExposureProfile()`, `q.UnilateralCVA()` en vez de strings sueltos. `.to_spec()` produce la
tupla `(nombre, params)` que ya acepta `Engine.calc`/`calc_batch`/`calc_many`/`calc_grid`
(`clients/python/src/engine_py_ext.cpp::to_measure_spec`) -- un string "pelado" sigue
funcionando igual (`eng.calc(product, ["PV", "DV01"], ...)`), `.to_spec()` es solo necesario
cuando una medida lleva configuración real.

    result = eng.calc(product, [q.PV().to_spec(), q.DV01(bump=0.0002).to_spec()], ...)
"""

from typing import ClassVar, Tuple

from pydantic import BaseModel, ConfigDict


class Measure(BaseModel):
    model_config = ConfigDict(frozen=True)

    measure_name: ClassVar[str]

    def to_params(self) -> dict:
        """Traduce este spec al `Params`/dict que consume la medida registrada subyacente."""
        raise NotImplementedError

    def to_spec(self) -> Tuple[str, dict]:
        """`(nombre, params)`: lo que espera `Engine.calc(...)` para pasar configuración por
        medida (PLAN_REAPI.md §6 Fase 3)."""
        return (self.measure_name, self.to_params())


class PV(Measure):
    """NPV determinista del trade (`engine::PresentValueMeasure`, PLAN.md §7.15). Sin
    configuración propia."""

    measure_name: ClassVar[str] = "PV"

    def to_params(self) -> dict:
        return {}


class DV01(Measure):
    """Sensibilidad del NPV a un movimiento de `bump` en r0 (`engine::Dv01Measure`). Primera
    medida con configuración real (PLAN_REAPI.md §6 Fase 3): `DV01(bump=0.0002).to_spec()` da
    un resultado distinto de `DV01().to_spec()` (default `bump=0.0001`, un punto básico)."""

    measure_name: ClassVar[str] = "DV01"

    bump: float = 0.0001

    def to_params(self) -> dict:
        return {"bump": self.bump}


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
