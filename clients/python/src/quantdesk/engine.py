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
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple, Union

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


@dataclass(frozen=True, slots=True)
class BatchRow:
    """Una fila de `Engine.price_batch`/`price_many` (§3.2): envuelve `engine.BatchResult`
    (campos nativos `trade_index`/`measures`, confirmados por introspección -- ver Fase 2 de
    PLAN_API_REFACTOR.md) igual que `PriceResult` envuelve el dict de `price(...)` -- `measures`
    pasa de `dict[str, engine.MeasureResult]` a `PriceResult` para tener acceso por punto
    también en batch/grid, no solo en `price(...)` individual."""

    trade_index: int
    measures: "PriceResult"


@dataclass(frozen=True, slots=True)
class GridRow:
    """Una celda de `Engine.price_grid` (§3.2): envuelve `engine.GridResult` (campos nativos
    `trade_index`/`model_index`/`market_index`/`measures`, confirmados por introspección) --
    mismo criterio que `BatchRow`."""

    trade_index: int
    model_index: int
    market_index: int
    measures: "PriceResult"


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

    # -- Traducción típed -> nativo (privado, compartido por todos los métodos de §3.2) ---------
    # Un único punto para `create_product`/`create_model`/`MarketSnapshot(**...)` (§3.2, "una sola
    # vez, dentro de Engine") -- exactamente el patrón que hoy escribe a mano cada script de
    # `clients/python/examples/`, factorizado para no repetirlo en price/price_batch/price_many/
    # price_grid/all_greeks/hessian/hvp/simulate_paths.

    def _to_native_product(self, trade: TradeSpec):
        return self._native.create_product(trade.product_type, trade.to_params())

    def _to_native_model(self, model: ModelSpec):
        return self._native.create_model(model.model_type, model.to_params())

    @staticmethod
    def _to_native_market(market: Market):
        return _native.MarketSnapshot(**market.to_params())

    def _resolve_pricing(self, pricing: Optional[PricingContext]) -> PricingContext:
        return pricing if pricing is not None else self._pricing

    def _resolve_execution(self, execution: Optional[ExecutionContext]) -> ExecutionContext:
        return execution if execution is not None else self._execution

    @staticmethod
    def _to_native_pricing(pricing: PricingContext):
        return _native.PricingContext(pricing.to_params())

    @staticmethod
    def _to_native_execution(execution: ExecutionContext):
        return _native.ExecutionContext(execution.to_params())

    @staticmethod
    def _to_native_metrics(metrics: Sequence[MetricSpec]) -> List:
        return [_metric_to_native(metric) for metric in metrics]

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
        native_product = self._to_native_product(trade)
        native_model = self._to_native_model(model)
        native_market = self._to_native_market(market)
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        native_execution = self._to_native_execution(self._resolve_execution(execution))
        native_metrics = self._to_native_metrics(metrics)

        results = self._native.price(
            native_product, native_metrics, native_model, native_market, native_pricing, native_execution
        )
        return PriceResult(results)

    def price_batch(
        self,
        trades: Sequence[TradeSpec],
        model: ModelSpec,
        market: Market,
        metrics: Sequence[MetricSpec],
        *,
        pricing: Optional[PricingContext] = None,
        execution: Optional[ExecutionContext] = None,
    ) -> List[BatchRow]:
        """Nivel 3 nativo (`engine.Engine.price_batch`, PLAN.md §7.17/§7.19): `trades` del mismo
        tipo/calendario, vectorizado sin bucle -- cada `IRSwap` debe traer `fixed_rate` explícito
        (sin `IRSwap.par(...)`, el lote nativo no lo soporta, igual que hoy). Una fila por trade,
        en el mismo orden que `trades`."""
        native_products = [self._to_native_product(trade) for trade in trades]
        native_model = self._to_native_model(model)
        native_market = self._to_native_market(market)
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        native_execution = self._to_native_execution(self._resolve_execution(execution))
        native_metrics = self._to_native_metrics(metrics)

        rows = self._native.price_batch(
            native_products, native_metrics, native_model, native_market, native_pricing, native_execution
        )
        return [BatchRow(trade_index=row.trade_index, measures=PriceResult(row.measures)) for row in rows]

    def price_many(
        self,
        trades: Sequence[TradeSpec],
        model: ModelSpec,
        market: Market,
        metrics: Sequence[MetricSpec],
        *,
        pricing: Optional[PricingContext] = None,
        execution: Optional[ExecutionContext] = None,
    ) -> List[BatchRow]:
        """Nivel 2 nativo (`engine.Engine.price_many`): igual que `price_batch` pero `trades`
        puede mezclar tipos/calendarios distintos -- el motor los agrupa internamente (nunca
        falla por heterogeneidad) y devuelve el resultado en el orden de entrada original."""
        native_products = [self._to_native_product(trade) for trade in trades]
        native_model = self._to_native_model(model)
        native_market = self._to_native_market(market)
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        native_execution = self._to_native_execution(self._resolve_execution(execution))
        native_metrics = self._to_native_metrics(metrics)

        rows = self._native.price_many(
            native_products, native_metrics, native_model, native_market, native_pricing, native_execution
        )
        return [BatchRow(trade_index=row.trade_index, measures=PriceResult(row.measures)) for row in rows]

    def price_grid(
        self,
        trades: Sequence[TradeSpec],
        models: Sequence[ModelSpec],
        markets: Sequence[Market],
        metrics: Sequence[MetricSpec],
        *,
        pricing: Optional[PricingContext] = None,
        execution: Optional[ExecutionContext] = None,
    ) -> List[GridRow]:
        """Explosión Trades x Models x Markets (`engine.Engine.price_grid`, PLAN.md §7.19): por
        cada par (modelo, mercado) llama internamente a `price_many` sobre `trades` entero.
        `pricing`/`execution` son compartidos por toda la rejilla, no forman parte de ella
        (mismo criterio que el binding nativo)."""
        native_products = [self._to_native_product(trade) for trade in trades]
        native_models = [self._to_native_model(model) for model in models]
        native_markets = [self._to_native_market(market) for market in markets]
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        native_execution = self._to_native_execution(self._resolve_execution(execution))
        native_metrics = self._to_native_metrics(metrics)

        cells = self._native.price_grid(
            native_products, native_metrics, native_models, native_markets, native_pricing, native_execution
        )
        return [
            GridRow(
                trade_index=cell.trade_index,
                model_index=cell.model_index,
                market_index=cell.market_index,
                measures=PriceResult(cell.measures),
            )
            for cell in cells
        ]

    def all_greeks(
        self,
        trade: TradeSpec,
        metric_name: str,
        model: ModelSpec,
        market: Market,
        *,
        metric_params: Optional[Dict[str, Any]] = None,
        include_curve_buckets: bool = False,
        include_second_order: bool = False,
        pricing: Optional[PricingContext] = None,
        execution: Optional[ExecutionContext] = None,
    ):
        """Barrido automático de Greeks de primer orden (`engine.Engine.all_greeks`,
        PLAN_GREEKS.md §8.5) sobre `metric_name`, sin enumerar spot/rate/dividend_yield/
        volatility/curva/crédito/tiempo a mano. Devuelve el `engine.GreeksReport` nativo tal
        cual (§3.3: no hace falta envolverlo, ya tiene atributos con nombre -- `.greeks`:
        `list[GreekResult]`, `.skipped`: `list[str]`).

        `pricing=`/`execution=` no están en la firma literal de §3.2 de PLAN_API_REFACTOR.md,
        pero el binding nativo los exige (posicionales, sin default) y
        `clients/python/notebooks/04_greeks_and_risk_surfaces.ipynb` ya varía `pricing` entre
        llamadas al `Engine` nativo dentro del mismo script (p.ej. `pricing` de 200k paths para
        la call GBM vs `hw_pricing` de 1k paths para el IRS Hull-White) -- mismo override
        puntual que ya ofrece `price(...)` (§2), añadido aquí por consistencia y para no
        obligar a instanciar un segundo `Engine` solo por esto. Ver Fase 2 de
        PLAN_API_REFACTOR.md, sección "Estado verificado / decisiones tomadas".
        """
        native_product = self._to_native_product(trade)
        native_model = self._to_native_model(model)
        native_market = self._to_native_market(market)
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        native_execution = self._to_native_execution(self._resolve_execution(execution))

        return self._native.all_greeks(
            native_product,
            metric_name,
            native_model,
            native_market,
            native_pricing,
            native_execution,
            metric_params if metric_params is not None else {},
            include_curve_buckets,
            include_second_order,
        )

    def hessian(
        self,
        trade: TradeSpec,
        metric_name: str,
        model: ModelSpec,
        market: Market,
        *,
        metric_params: Optional[Dict[str, Any]] = None,
        risk_factors: Optional[Sequence[str]] = None,
        pricing: Optional[PricingContext] = None,
        execution: Optional[ExecutionContext] = None,
    ):
        """Hessiano local completo (`engine.Engine.hessian`, PLAN_BACKWARD.md §7-9 Fase 1-3):
        `risk_factors=None` enumera automáticamente los candidatos soportados (mismo criterio
        que `all_greeks`); una `list[str]` namespaced (`"model.spot"`, ...) pide solo esos
        factores. Devuelve el `engine.HessianReport` nativo tal cual (`.entries`: triángulo
        superior + diagonal, `.skipped`). `pricing=`/`execution=`: mismo override puntual y
        misma justificación que en `all_greeks` (ver docstring de `all_greeks`)."""
        native_product = self._to_native_product(trade)
        native_model = self._to_native_model(model)
        native_market = self._to_native_market(market)
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        native_execution = self._to_native_execution(self._resolve_execution(execution))

        return self._native.hessian(
            native_product,
            metric_name,
            native_model,
            native_market,
            native_pricing,
            native_execution,
            metric_params if metric_params is not None else {},
            risk_factors,
        )

    def hvp(
        self,
        trade: TradeSpec,
        metric_name: str,
        model: ModelSpec,
        market: Market,
        direction: Dict[str, float],
        *,
        metric_params: Optional[Dict[str, Any]] = None,
        pricing: Optional[PricingContext] = None,
        execution: Optional[ExecutionContext] = None,
    ):
        """Producto Hessiano-vector H*v (`engine.Engine.hvp`, PLAN_BACKWARD.md §7-9 Fase 3):
        `direction` es un dict disperso `{"model.spot": 1.0, ...}` (factores ausentes = peso 0).
        Devuelve el `engine.HvpReport` nativo tal cual (`.components`, `.skipped`). `pricing=`/
        `execution=`: mismo override puntual y misma justificación que en `all_greeks`."""
        native_product = self._to_native_product(trade)
        native_model = self._to_native_model(model)
        native_market = self._to_native_market(market)
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        native_execution = self._to_native_execution(self._resolve_execution(execution))

        return self._native.hvp(
            native_product,
            metric_name,
            native_model,
            native_market,
            native_pricing,
            native_execution,
            direction,
            metric_params if metric_params is not None else {},
        )

    def simulate_paths(self, model: ModelSpec, market: Market, *, pricing: Optional[PricingContext] = None):
        """Diagnóstico de trayectorias Monte Carlo (`engine.Engine.simulate_paths`,
        PLAN_IMPROVE_NOTEBOOK.md Fase 0) -- NO es una medida de `price(...)`. Devuelve
        `(times, paths)` tal cual el nativo (arrays `numpy`, §3.3: nada que envolver). Solo
        modelos que generan un observable simulable (`GBM`/`GBM_P`/`GbmBasket`); el horizonte
        `T` es siempre `market.pillars()[-1]`.

        `pricing=` no está en la firma literal de §3.2 (que solo lista `(model, market)`), pero
        el binding nativo lo exige como tercer argumento posicional (no hay `execution` en
        `simulate_paths` nativo, a diferencia del resto de métodos) y
        `clients/python/notebooks/07_montecarlo_paths_q_vs_p.ipynb` llama a `simulate_paths` dos
        veces en el mismo script con `PricingContext` distintos (`pricing_q`/`pricing_p`: mismo
        `n_paths`/`n_steps` pero semillas distintas). Sin override puntual, `quantdesk.Engine`
        solo podría reproducir ese notebook creando un segundo `Engine` -- se añade el mismo
        patrón `pricing=None -> self._pricing` que ya usa `price(...)`. Ver Fase 2 de
        PLAN_API_REFACTOR.md, sección "Estado verificado / decisiones tomadas"."""
        native_model = self._to_native_model(model)
        native_market = self._to_native_market(market)
        native_pricing = self._to_native_pricing(self._resolve_pricing(pricing))
        return self._native.simulate_paths(native_model, native_market, native_pricing)

    def evaluate_scenario(self, trade: TradeSpec, scenario: Dict[str, float]) -> List[Tuple[float, str, float]]:
        """Evaluación DETERMINISTA de un `PayoffProduct` sobre un escenario de mercado fijo
        (`engine.Engine.evaluate_scenario`, PLAN_IMPROVE_NOTEBOOK2.md Fase 1) -- sin modelo, sin
        Monte Carlo, sin descuento: ejecuta el AST ya compilado (`ScenarioEvaluator`) sobre una
        única ruta conocida. Devuelve el ledger crudo `list[(time, currency, amount)]` tal cual
        el nativo (§3.3: nada que envolver).

        Añadido en PLAN_API_REFACTOR.md Fase 5, no en la Fase 2 original: `evaluate_scenario` se
        incorporó al binding nativo (`engine.Engine`) en PLAN_IMPROVE_NOTEBOOK2.md Fase 1, que se
        implementó DESPUÉS de que la Fase 2 de este plan introspeccionara y envolviera el resto
        de métodos de `engine.Engine` -- es un hueco real de cobertura (el criterio de aceptación
        de la Fase 2, "ninguna capacidad de la fachada dinámica queda solo alcanzable importando
        `engine` a mano", habría exigido cubrirlo si el método ya hubiera existido entonces), no
        una omisión de diseño de esta fase. Encontrado al migrar
        `09_option_strategies_and_greeks.ipynb`, que lo usa para el payoff intrínseco de cada
        estrategia sin reimplementar `max(S-K,0)`/`max(K-S,0)` a mano en NumPy. Mismo patrón de
        traducción típed -> nativo que el resto de métodos (`_to_native_product`); `scenario` ya
        es un dict `{observable: spot}`, sin objeto tipado que traducir."""
        native_product = self._to_native_product(trade)
        return self._native.evaluate_scenario(native_product, scenario)

    def calibrate(self, model_type: str, market: Market, initial_guess: Dict[str, float]):
        """`create_calibrator(model_type) + .calibrate(market, initial_guess)` en un paso
        (§3.2). `market` es el objeto tipado `quantdesk.market.Market`, traducido aquí igual
        que en el resto de métodos; `initial_guess` es el dict de parámetros iniciales, mismas
        claves que `create_model` para `model_type` (idéntico al uso nativo hoy, sin cambio de
        forma). Devuelve el `engine.CalibrationResult` nativo tal cual (§3.3: `.optimal_params`
        se puede pasar directamente a `create_model`/`quantdesk.model.ModelSpec`)."""
        native_market = self._to_native_market(market)
        calibrator = self._native.create_calibrator(model_type)
        return calibrator.calibrate(native_market, initial_guess)

    def list_models(self) -> List[str]:
        """Nombres de modelo registrados (`engine.Engine.list_models`), tal cual."""
        return self._native.list_models()

    def list_products(self) -> List[str]:
        """Nombres de producto registrados (`engine.Engine.list_products`), tal cual."""
        return self._native.list_products()

    def list_measures(self) -> List[str]:
        """Nombres de medida registrados (`engine.Engine.list_measures`), tal cual."""
        return self._native.list_measures()

    def list_calibrators(self) -> List[str]:
        """Nombres de calibrador registrados (`engine.Engine.list_calibrators`), tal cual."""
        return self._native.list_calibrators()
