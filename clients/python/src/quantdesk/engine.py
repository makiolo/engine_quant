"""`quantdesk.Engine` + `PriceResult` (PLAN_API_REFACTOR.md Fase 1, §3.2/§3.3): la fachada nueva
que sustituye a la "doble construcción" típed -> nativo que hoy escribe cada script a mano
(`clients/python/examples/price_flow.py`, `price_flow_typed.py`) -- `Engine` envuelve una
instancia de `engine.Engine()` nativa más un `PricingContext`/`ExecutionContext` ya resueltos en
el constructor, y traduce trade/model/market tipados (`quantdesk.trade.TradeSpec`,
`quantdesk.model.ModelSpec`, `quantdesk.market.Market`) al mismo `Params`/dict que ya consumía
ese patrón, una sola vez, dentro de `price(...)`.

Nombre del módulo: `quantdesk.engine` (no colisiona con el paquete top-level `engine`, el módulo
nanobind compilado -- `import engine` dentro de este fichero resuelve a ese paquete top-level,
no a sí mismo, porque un import absoluto en Python nunca mira dentro del propio paquete
contenedor, `quantdesk`, para resolver un nombre de primer nivel).

    from quantdesk import Engine, HullWhite1F, IRSwap, Market

    model = HullWhite1F(a=0.10, b=0.03, sigma=0.01, r0=0.02)
    trade = IRSwap(notional=1_000_000.0, fixed_rate=0.02,
                    payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0] * 5)
    market = Market(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02],
                     hazard_rate=0.02, recovery_rate=0.40)

    engine = Engine(backend="auto", n_paths=5_000, n_steps=208, seed=7)
    metrics = ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"]
    results = engine.price(trade, model, market, metrics)
    print(results.PV.scalar)
"""

from collections.abc import Mapping
from typing import List, Optional, Sequence, Union

import engine as _native

from quantdesk.context import ExecutionContext, PricingContext
from quantdesk.market import Market
from quantdesk.measure import Measure
from quantdesk.model import ModelSpec
from quantdesk.trade import TradeSpec

MetricSpec = Union[str, Measure]


class PriceResult(Mapping):
    """Envoltorio de solo lectura sobre `dict[str, engine.MeasureResult]` (§3.3): acceso por
    punto (`results.PV.scalar`) ADEMÁS del acceso por string ya existente (`results["PV"].scalar`)
    -- mismo objeto `engine.MeasureResult` nativo en ambos casos, sin reimplementar
    `scalar`/`times`/`primary`/`secondary`/`bump_used`/`has_scalar` (son atributos del
    `MeasureResult` nativo tal cual los devuelve `engine.Engine.price(...)`).

    `Mapping[str, engine.MeasureResult]` -- `len()`, `in`, `.keys()`/`.values()`/`.items()`,
    iteración, siguen funcionando igual que sobre el dict que envuelve.
    """

    __slots__ = ("_results",)

    def __init__(self, results: dict):
        self._results = results

    def __getitem__(self, key: str):
        return self._results[key]

    def __iter__(self):
        return iter(self._results)

    def __len__(self) -> int:
        return len(self._results)

    def __repr__(self) -> str:
        return f"PriceResult({self._results!r})"

    def __getattr__(self, name: str):
        # Solo se llama cuando el atributo NO existe ya por la vía normal (__slots__, métodos de
        # Mapping, etc.) -- así que esto es exclusivamente el acceso por punto a una medida
        # (`results.PV`), nunca compite con `_results` o con los métodos de Mapping.
        try:
            return self._results[name]
        except KeyError:
            raise AttributeError(
                f"PriceResult no tiene la medida {name!r} -- medidas disponibles: {sorted(self._results)}"
            ) from None


def _metric_to_native(metric: MetricSpec):
    """`x.to_spec() if isinstance(x, Measure) else x` por elemento (§2), igual que ya hace
    `engine.Engine.price` internamente con tuplas `(nombre, params)`/`(nombre, params, alias)` --
    ver el docstring de `quantdesk.measure.Measure.to_spec`."""
    return metric.to_spec() if isinstance(metric, Measure) else metric


class Engine:
    """Fachada tipada sobre `engine.Engine()` (PLAN_API_REFACTOR.md §3.2): fija
    `PricingContext`/`ExecutionContext` una vez en el constructor (configuración del motor, no
    del trade) y traduce trade/model/market tipados a los objetos nativos que ya espera
    `engine.Engine.price(product, measure_names, model, market, pricing, execution)` --
    mismo patrón de traducción que hoy escribe a mano cada script de
    `clients/python/examples/` (`price_flow.py`, `price_flow_typed.py`), una sola vez.
    """

    def __init__(
        self,
        *,
        backend: str = "auto",
        precision: str = "FP64",
        n_paths: int,
        n_steps: int,
        seed: int,
        pricing_date: float = 0.0,
    ):
        self._native = _native.Engine()
        self._pricing = PricingContext(pricing_date=pricing_date, n_paths=n_paths, n_steps=n_steps, seed=seed)
        self._execution = ExecutionContext(backend=backend, precision=precision)

    def price(
        self,
        trade: TradeSpec,
        model: ModelSpec,
        market: Market,
        metrics: Sequence[MetricSpec],
        *,
        pricing: Optional[PricingContext] = None,
        execution: Optional[ExecutionContext] = None,
    ) -> PriceResult:
        """Calcula `metrics` sobre `trade`/`model`/`market` (§3.2). `pricing=`/`execution=`
        sobreescriben puntualmente el `PricingContext`/`ExecutionContext` fijados en
        `Engine.__init__` SOLO para esta llamada -- no mutan `self._pricing`/`self._execution`,
        así que una llamada siguiente sin override sigue usando la configuración del constructor
        (§2, verificado explícitamente en la Fase 1 de PLAN_API_REFACTOR.md).

        `trade`/`model`/`market` son siempre objetos tipados (`quantdesk.trade.TradeSpec`,
        `quantdesk.model.ModelSpec`, `quantdesk.market.Market`) -- se traducen aquí a
        `engine.Product`/`engine.Model`/`engine.MarketSnapshot` exactamente como ya hacía cada
        script a mano. `metrics` acepta mezcla de medidas tipadas (`q.PV()`, `q.DV01(bump=...)`)
        y strings, igual que `engine.Engine.price` hoy.
        """
        native_product = self._native.create_product(trade.product_type, trade.to_params())
        native_model = self._native.create_model(model.model_type, model.to_params())
        native_market = _native.MarketSnapshot(**market.to_params())
        active_pricing = pricing if pricing is not None else self._pricing
        active_execution = execution if execution is not None else self._execution
        native_pricing = _native.PricingContext(active_pricing.to_params())
        native_execution = _native.ExecutionContext(active_execution.to_params())

        native_metrics: List = [_metric_to_native(metric) for metric in metrics]

        results = self._native.price(
            native_product, native_metrics, native_model, native_market, native_pricing, native_execution
        )
        return PriceResult(results)
