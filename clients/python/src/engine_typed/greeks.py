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

    def to_params(self) -> dict:
        params: dict = {
            "metric": self.metric,
            "risk_factor": self.risk_factor,
            "order": float(self.order),
            "method": self.method,
        }
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


#: Bump temporal por defecto para `theta()`/`risk_factor="time.theta"`, en anios (un dia).
#: DEBE coincidir con `default_time_shift_bump()` en `cpp/engine/src/greeks.cpp` -- es un
#: default que hoy vive en dos lenguajes porque `engine.MeasureResult` (lo que devuelve
#: `Engine.price(...)["Greek"]`, el camino que usa `theta()`) no expone `bump_used` -- a
#: diferencia de `engine.GreekResult` (lo que devuelve `Engine.all_greeks(...)`), que si lo
#: expone. Cerrar esa asimetria es cambio de bridge C++ (`engine_py_ext.cpp`), fuera de
#: alcance de PLAN_IMPROVE_NOTEBOOK.md Fase 4 (documentacion/API pura). Mientras tanto,
#: `theta(annualized=True)` con `bump=None` resuelve el bump a ESTE valor y lo manda
#: explicito en el spec (ver `theta()` abajo) -- no confia en que el default interno del
#: motor coincida por convencion: si algun dia diverge de `default_time_shift_bump()`, el
#: caso `bump=None` sigue siendo exacto por construccion (el motor usa el bump que se le
#: pide, no el suyo propio), lo unico que podria desalinearse es el caso `annualized=False`
#: con `bump=None` (que no depende de esta constante en absoluto).
DEFAULT_THETA_BUMP: float = 1.0 / 365.0


class ThetaGreek(Greek):
    """Subclase de `Greek` que devuelve `theta()` (PLAN_IMPROVE_NOTEBOOK.md Fase 4, ADR-IN-01
    en ese documento). `to_spec()`/`to_params()` se heredan sin cambios -- el motor no conoce
    `annualized`, nunca viaja en el spec (no es un parametro de la medida "Greek" en C++, es
    puro post-proceso Python sobre el escalar que ya devolvio el motor). `annualize()` es ese
    post-proceso: lo aplica el LLAMANTE explicitamente sobre el valor leido de
    `MeasureResult.scalar`, porque `Engine.price(...)` no pasa por esta clase para construir
    el resultado (solo consume `to_spec()`)."""

    annualized: bool = False

    def annualize(self, raw_value: float) -> float:
        """Aplica la convencion elegida en el ADR: si `annualized` es False (default), `raw_value`
        se devuelve tal cual (ΔV crudo del bump, la convencion por defecto de `theta()`). Si es
        True, devuelve `raw_value / self.bump` (derivada anualizada dV/dt) -- `self.bump` es
        siempre un float concreto en este caso (`theta()` lo resuelve a `DEFAULT_THETA_BUMP` si
        el llamante no dio uno explicito), nunca `None`, precisamente para que esta division use
        EL MISMO bump que el motor uso para calcular `raw_value`."""
        if not self.annualized:
            return raw_value
        assert self.bump is not None  # invariante garantizado por theta(), ver abajo
        return raw_value / self.bump


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
    dia calendario real (`DEFAULT_THETA_BUMP` = 1/365 anios), no un bump infinitesimal elegido
    solo por estabilidad numerica como en `delta`/`vega`/`rho`/`dv01`. Es la UNICA Greek con esta
    convencion (esas otras cuatro si son derivadas propiamente normalizadas por el tamano del
    bump).

    `annualized=True` (nuevo en esta fase) devuelve en cambio `ΔV / dt`, es decir, la derivada
    anualizada `dV/dt` -- equivalente a lo que hace el llamante hoy a mano multiplicando/
    dividiendo por `bump_days` fuera de la API (ver antes/despues en
    `01_vanilla_options_black_scholes.ipynb` §6). El escalado ocurre en Python DESPUES de que el
    motor devuelva el resultado (`ThetaGreek.annualize(raw_value)`, ver esa clase) -- `theta()`
    en si solo construye el spec, no llama al motor, asi que el llamante sigue el patron
    `eng.price(product, [greek.to_spec()], ...)["Greek"].scalar` habitual y aplica
    `greek.annualize(...)` sobre ese escalar; no hace falta que sepa el valor concreto de
    `bump` para hacerlo bien (evita duplicar el numero magico `1/365` en el notebook).

    `bump` aqui es `dt` (anios) -- default un dia (`DEFAULT_THETA_BUMP` = 1/365) si se omite.
    Con `annualized=True` y `bump=None`, se resuelve `DEFAULT_THETA_BUMP` y se manda EXPLICITO
    en el spec (a diferencia del caso `annualized=False`, donde `bump=None` se deja pasar tal
    cual y es el motor quien aplica su propio default) -- asi el bump que usa el motor para
    calcular y el bump por el que se divide son garantizadamente el mismo, sin depender de que
    el default de Python y el de `cpp/engine/src/greeks.cpp::default_time_shift_bump()` sigan
    coincidiendo por convencion."""
    effective_bump = bump
    if annualized and effective_bump is None:
        effective_bump = DEFAULT_THETA_BUMP
    return ThetaGreek(
        metric=metric, risk_factor="time.theta", metric_params=metric_params, bump=effective_bump, annualized=annualized
    )


def hazard_rate(metric: str = "UnilateralCVA", *, bump: Optional[float] = None, **metric_params) -> Greek:
    """Sensibilidad de credito (PLAN_GREEKS.md §11 Fase 4): `metric` por defecto
    "UnilateralCVA", la metrica que de verdad consume `hazard_rate`/`recovery_rate`."""
    return Greek(metric=metric, risk_factor="credit.hazard_rate", metric_params=metric_params, bump=bump)


def recovery_rate(metric: str = "UnilateralCVA", *, bump: Optional[float] = None, **metric_params) -> Greek:
    return Greek(metric=metric, risk_factor="credit.recovery_rate", metric_params=metric_params, bump=bump)
