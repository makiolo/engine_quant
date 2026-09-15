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


def theta(metric: str = "PV", *, bump: Optional[float] = None, **metric_params) -> Greek:
    """Theta puro (PLAN_GREEKS.md §7.1): `metric` por defecto "PV" (junto con "PayoffPriceQ",
    una de las dos metricas que honran `PricingContext::pricing_date()` hoy, §11 Fase 5).
    `bump` aqui es `dt` (anios) -- default un dia (1/365) si se omite."""
    return Greek(metric=metric, risk_factor="time.theta", metric_params=metric_params, bump=bump)


def hazard_rate(metric: str = "UnilateralCVA", *, bump: Optional[float] = None, **metric_params) -> Greek:
    """Sensibilidad de credito (PLAN_GREEKS.md §11 Fase 4): `metric` por defecto
    "UnilateralCVA", la metrica que de verdad consume `hazard_rate`/`recovery_rate`."""
    return Greek(metric=metric, risk_factor="credit.hazard_rate", metric_params=metric_params, bump=bump)


def recovery_rate(metric: str = "UnilateralCVA", *, bump: Optional[float] = None, **metric_params) -> Greek:
    return Greek(metric=metric, risk_factor="credit.recovery_rate", metric_params=metric_params, bump=bump)
