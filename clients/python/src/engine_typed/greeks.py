"""`engine_typed.greeks`: fachada tipada sobre la medida generica "Greek" (PLAN_GREEKS.md
§8.1/§8.2/§8.4). `Greek` traduce `metric`/`risk_factor`/`order`/`method`/`bump` (mas los
parametros propios de la metrica interior, via la convencion de prefijo `metric.*`) al mismo
`Params`/dict que ya consume `Engine.price`/`price_batch`/`price_many`/`price_grid` -- igual
patron `.to_spec()` que `engine_typed.measure.PV`/`DV01`. Los builders `delta`/`vega`/`rho`/
`gamma`/`dv01`/`theta`/`hazard_rate`/`recovery_rate` son azucar sobre `Greek` para los
`RiskFactor` mas comunes (§8.4: "greeks.delta('spot')", etc.), no una API distinta -- todos
aceptan `bump`/`method` opcionales ademas de los parametros propios de la metrica interior.

Una Greek concreta (una combinacion metric/risk_factor/order/method a la vez) es esta clase;
el BARRIDO automatico de todas las Greeks aplicables a una metrica (§8.5, sin enumerar cada
parametro de modelo/curva/credito/tiempo a mano) es `Engine.all_greeks(...)`, no esta clase --
ver PLAN_GREEKS.md §9.1 para el flujo completo autoria -> price -> all_greeks.

    import engine, engine_typed as q
    from engine_typed import greeks

    eng = engine.Engine()
    ...
    result = eng.price(product, [greeks.delta("PayoffPriceQ", "spot").to_spec()], ...)
"""

from typing import ClassVar, Optional

from engine_typed.measure import Measure


class Greek(Measure):
    """Sensibilidad de `metric` (nombre de una medida ya registrada en el motor, p.ej.
    "PayoffPriceQ"/"PV"/"UnilateralCVA") respecto de `risk_factor` (string namespaced,
    PLAN_GREEKS.md §3.1: "model.<param>", "curve.parallel", "curve.pillar:<i>",
    "credit.hazard_rate"/"credit.recovery_rate", "time.theta"). `metric` es siempre explicito
    -- una Greek no tiene sentido sin saber a que metrica se refiere (§0.1), asi que no hay
    default silencioso aqui (los builders de abajo si eligen uno razonable para su caso de uso
    concreto). `metric_params` son los propios de la metrica interior (p.ej. `event` de
    PayoffHitProbabilityQ, `confidence` de PayoffPnlDistributionP) -- se reenvian con el
    prefijo `metric.*` (§8.2) sin que el llamante tenga que saber que `Params` es un bag plano
    sin anidamiento."""

    measure_name: ClassVar[str] = "Greek"

    metric: str
    risk_factor: str
    metric_params: dict = {}
    order: int = 1
    method: str = "auto"
    bump: Optional[float] = None
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 4: string namespaced opcional (mismo formato que
    # risk_factor, p.ej. "model.spot_1") que habilita la derivada CRUZADA de 4 puntos que ya
    # calcula `compute_greek` (Vanna, o para GbmBasket la cross-gamma real entre dos activos) --
    # ver `cross_gamma()` mas abajo para el builder "amigable" equivalente a `delta()`/`gamma()`.
    cross_factor: Optional[str] = None

    def to_params(self) -> dict:
        params: dict = {
            "metric": self.metric,
            "risk_factor": self.risk_factor,
            "order": float(self.order),
            "method": self.method,
        }
        if self.cross_factor is not None:
            params["cross_factor"] = self.cross_factor
        for key, value in self.metric_params.items():
            params[f"metric.{key}"] = value
        if self.bump is not None:
            params["bump"] = self.bump
        return params


def delta(
    metric: str, risk_factor: str, *, bump: Optional[float] = None, method: str = "auto", **metric_params
) -> Greek:
    """Derivada de primer orden de `metric` respecto de un parametro de MODELO -- p.ej.
    `delta("PayoffPriceQ", "spot")` -> `risk_factor="model.spot"`. `risk_factor` es el nombre
    "amigable" del parametro (sin el prefijo "model.", que añade esta funcion), mismo criterio
    que ya usa `compute_greek` del lado C++ (ver `resolve_model_parameter_key` en greeks.cpp).
    `bump`/`method` (PLAN_GREEKS.md §3.4/§3.3) son opcionales: `bump=None` usa la politica de
    default por tipo de factor, `method="auto"` (default) elige la ruta especializada
    verificada si existe, o cae a bump-and-reval."""
    return Greek(metric=metric, risk_factor=f"model.{risk_factor}", metric_params=metric_params, bump=bump, method=method)


def vega(
    metric: str, risk_factor: str = "volatility", *, bump: Optional[float] = None, method: str = "auto", **metric_params
) -> Greek:
    """Delta del parametro de volatilidad -- `risk_factor` por defecto es "volatility" (Gbm/
    GbmP), se puede sobreescribir para un modelo con otro nombre de parametro de vol."""
    return delta(metric, risk_factor, bump=bump, method=method, **metric_params)


def rho(
    metric: str, risk_factor: str = "rate", *, bump: Optional[float] = None, method: str = "auto", **metric_params
) -> Greek:
    """Delta del parametro de tipo de interes del propio modelo (distinto de `dv01`, que es la
    sensibilidad a la CURVA de mercado, no a un parametro de `IModel`) -- `risk_factor` por
    defecto es "rate" (Gbm), se puede sobreescribir (p.ej. "r0" para Hull-White)."""
    return delta(metric, risk_factor, bump=bump, method=method, **metric_params)


def gamma(metric: str, risk_factor: str, *, bump: Optional[float] = None, **metric_params) -> Greek:
    """Segunda derivada pura (PLAN_GREEKS.md §11 Fase 6) del mismo `risk_factor` que `delta` --
    mismo estencil de 3 puntos que ya usa `compute_greek` cuando `order=2`. Sin `method`: las
    rutas especializadas solo cubren `order=1` (§5.2), Gamma siempre es bump-and-reval."""
    return delta(metric, risk_factor, bump=bump, **metric_params).model_copy(update={"order": 2})


def cross_gamma(
    metric: str, risk_factor: str, cross_risk_factor: str, *, bump: Optional[float] = None, **metric_params
) -> Greek:
    """Derivada cruzada de primer orden (Vanna, o -- PLAN_IMPROVE_NOTEBOOK2.md Fase 4 -- la
    cross-gamma real entre dos activos de un `GbmBasket`, `d^2V/dS_i dS_j`) via el estencil
    generico de 4 puntos de `compute_greek` (`order=1` con `cross_factor` poblado, ver el
    doc-comment de `engine::greeks::GreekMeasure` en `greeks.hpp`). `risk_factor`/
    `cross_risk_factor` son nombres "amigables" SIN el prefijo "model." (mismo criterio que
    `delta`) -- p.ej. `cross_gamma("PayoffPriceQ", "spot_0", "spot_1")` para la cross-gamma de un
    basket de 2 activos. Sin `method`: la especializacion pathwise (Vanna de GBM/GBM_P via
    likelihood ratio) se intenta primero automaticamente bajo `method="auto"` (default) si aplica
    -- para GbmBasket ninguna especializacion esta cableada todavia (PLAN_IMPROVE_NOTEBOOK2.md
    Fase 4, decision documentada: bump-and-reval generico, no una ruta pathwise nueva), asi que
    siempre cae al estencil de 4 puntos."""
    base = delta(metric, risk_factor, bump=bump, **metric_params)
    return base.model_copy(update={"cross_factor": f"model.{cross_risk_factor}"})


def dv01(
    metric: str = "PV", pillar: Optional[int] = None, *, bump: Optional[float] = None, **metric_params
) -> Greek:
    """Sensibilidad a la curva de descuento (PLAN_GREEKS.md §3.1): `curve.parallel` por
    defecto, o `curve.pillar:<i>` si se pasa `pillar`. `metric` por defecto es "PV" -- a
    diferencia de `delta`/`vega`/`rho`, una sensibilidad de curva sobre un NPV es el caso de
    uso abrumadoramente mas comun (equivalente al `Dv01Measure` heredado, PLAN_GREEKS.md §10),
    asi que aqui si hay un default razonable. Para el barrido de TODOS los pillars a la vez
    (bucketed) usar `Engine.all_greeks(..., include_curve_buckets=True)`, no esta funcion."""
    risk_factor = "curve.parallel" if pillar is None else f"curve.pillar:{pillar}"
    return Greek(metric=metric, risk_factor=risk_factor, metric_params=metric_params, bump=bump)


class ThetaGreek(Greek):
    """Subclase de `Greek` que devuelve `theta()` (PLAN_IMPROVE_NOTEBOOK.md Fase 4, ADR-IN-01
    en ese documento). `to_spec()`/`to_params()` se heredan sin cambios -- el motor no conoce
    `annualized`, nunca viaja en el spec (no es un parametro de la medida "Greek" en C++, es
    puro post-proceso Python sobre el resultado que ya devolvio el motor). `annualize()` es ese
    post-proceso: lo aplica el LLAMANTE explicitamente sobre el `MeasureResult` leido de
    `Engine.price(...)["Greek"]`, porque `Engine.price(...)` no pasa por esta clase para
    construir el resultado (solo consume `to_spec()`).

    PLAN_IMPROVE_NOTEBOOK2.md Fase 5: `annualize()` recibe el `MeasureResult` COMPLETO (no solo
    `.scalar`) y lee `bump_used` de ahi -- el motor lo puebla con el bump EFECTIVAMENTE usado
    para el time shift (`GreekMeasure::evaluate`, `cpp/engine/src/greeks.cpp`), resuelto
    internamente cuando el llamante paso `bump=None`. Antes de esta fase, `engine.MeasureResult`
    no exponia ese dato (a diferencia de `engine.GreekResult`, lo que devuelve
    `Engine.all_greeks(...)`, que si lo hacia via `bump_used`) y este modulo mantenia una
    constante Python duplicada (`DEFAULT_THETA_BUMP`, retirada en esta fase) que DEBIA coincidir
    a mano con `default_time_shift_bump()` en `cpp/engine/src/greeks.cpp` -- un acoplamiento
    cruzado Python/C++ que ya no hace falta."""

    annualized: bool = False

    def annualize(self, measure_result) -> float:
        """Aplica la convencion elegida en el ADR sobre `measure_result` (el `engine.MeasureResult`
        que devolvio `eng.price(product, [self.to_spec()], ...)["Greek"]`): si `annualized` es
        False (default), `measure_result.scalar` se devuelve tal cual (ΔV crudo del bump, la
        convencion por defecto de `theta()`). Si es True, devuelve
        `measure_result.scalar / measure_result.bump_used` (derivada anualizada dV/dt) --
        `bump_used` es el bump REAL que el motor aplico para esta Greek concreta (nunca una
        constante Python que pudiera desalinearse del default interno del motor)."""
        if not self.annualized:
            return measure_result.scalar
        bump_used = measure_result.bump_used
        assert bump_used is not None, (
            "MeasureResult.bump_used ausente para una Greek risk_factor='time.theta' -- el motor "
            "siempre resuelve theta via bump-and-reval (TimeShift no tiene ruta AAD/pathwise, ver "
            "metric_supports_time_shift en greeks.cpp); si esto salta indica una regresion del "
            "motor, no un caso legitimo de bump_used ausente."
        )
        return measure_result.scalar / bump_used


def theta(
    metric: str = "PV", *, bump: Optional[float] = None, annualized: bool = False, **metric_params
) -> ThetaGreek:
    """Theta puro (PLAN_GREEKS.md §7.1): `metric` por defecto "PV" (junto con "PayoffPriceQ",
    una de las dos metricas que honran `PricingContext::pricing_date()` hoy, §11 Fase 5).

    Convencion devuelta (PLAN_IMPROVE_NOTEBOOK.md Fase 4, ADR-IN-01 -- decision explicita, no
    inferida empiricamente): por defecto (`annualized=False`) el motor calcula `V(t+dt) - V(t)`
    para un unico bump `dt` (anios, `bump` aqui) y ESE ΔV crudo es lo que se devuelve --
    NO la derivada anualizada `dV/dt` -- porque para un desk es mas intuitivo como "P&L de
    revalorizar la cartera un dia" que como una tasa por anio, y porque `dt` por defecto es un
    dia calendario real (un dia = 1/365 anios, el default interno del motor,
    `default_time_shift_bump()` en `cpp/engine/src/greeks.cpp`), no un bump infinitesimal elegido
    solo por estabilidad numerica como en `delta`/`vega`/`rho`/`dv01`. Es la UNICA Greek con esta
    convencion (esas otras cuatro si son derivadas propiamente normalizadas por el tamano del
    bump).

    `annualized=True` (nuevo en `PLAN_IMPROVE_NOTEBOOK.md` Fase 4) devuelve en cambio `ΔV / dt`,
    es decir, la derivada anualizada `dV/dt` -- equivalente a lo que hace el llamante hoy a mano
    multiplicando/dividiendo por `bump_days` fuera de la API. El escalado ocurre en Python
    DESPUES de que el motor devuelva el resultado (`ThetaGreek.annualize(measure_result)`, ver
    esa clase) -- `theta()` en si solo construye el spec, no llama al motor, asi que el llamante
    sigue el patron `eng.price(product, [greek.to_spec()], ...)["Greek"]` habitual y aplica
    `greek.annualize(...)` sobre ese `MeasureResult`.

    `bump` aqui es `dt` (anios) -- `None` (default) se deja pasar TAL CUAL en el spec, sea cual
    sea `annualized`: es el motor quien resuelve su propio default interno
    (`default_time_shift_bump()`) y lo reporta de vuelta en `MeasureResult.bump_used`
    (PLAN_IMPROVE_NOTEBOOK2.md Fase 5) -- `ThetaGreek.annualize()` divide por ESE valor, nunca
    por una constante Python que pudiera desalinearse del default real del motor. Antes de esa
    fase, `annualized=True` con `bump=None` tenia que resolver el bump a mano en Python
    (`DEFAULT_THETA_BUMP`, retirada) precisamente porque `engine.MeasureResult` no exponia el
    bump efectivo -- ya no hace falta."""
    return ThetaGreek(
        metric=metric, risk_factor="time.theta", metric_params=metric_params, bump=bump, annualized=annualized
    )


def hazard_rate(metric: str = "UnilateralCVA", *, bump: Optional[float] = None, **metric_params) -> Greek:
    """Sensibilidad de credito (PLAN_GREEKS.md §11 Fase 4): `metric` por defecto
    "UnilateralCVA", la metrica que de verdad consume `hazard_rate`/`recovery_rate`."""
    return Greek(metric=metric, risk_factor="credit.hazard_rate", metric_params=metric_params, bump=bump)


def recovery_rate(metric: str = "UnilateralCVA", *, bump: Optional[float] = None, **metric_params) -> Greek:
    return Greek(metric=metric, risk_factor="credit.recovery_rate", metric_params=metric_params, bump=bump)
