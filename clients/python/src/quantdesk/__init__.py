"""`quantdesk`: fachada Python tipada sobre `engine` (PLAN_REAPI.md, propuestas 1-3).

Paquete Python puro, aditivo: traduce objetos `pydantic` (`TradeSpec`/`IRSwap`, `Model`/
`HullWhite1F`/`HullWhite2F`, `Market`, `PricingContext`, `ExecutionContext`, y en fases
siguientes `Measure`) al mismo `Params`/dict que ya consume `engine.Engine` -- no sustituye la
fachada dinámica (dict/Excel/C ABI), es una fachada más sobre el mismo registry C++
(PLAN_REAPI.md §2, mismo patrón que el segundo constructor de `MarketSnapshot`).

    import engine, quantdesk as q

    trade = q.IRSwap(
        notional=1_000_000.0, fixed_rate=q.PAR,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    market = q.Market(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    pricing = q.PricingContext(n_paths=5000, n_steps=208, seed=7)
    execution = q.ExecutionContext(backend="auto")

    eng = engine.Engine()
    product = eng.create_product(trade.product_type, trade.to_params())
    eng_model = eng.create_model(model.model_type, model.to_params())
    eng_market = engine.MarketSnapshot(**market.to_params())
    eng_pricing = engine.PricingContext(pricing.to_params())
    eng_execution = engine.ExecutionContext(execution.to_params())
"""

import importlib.util

from quantdesk.rest import (
    Context,
    HullWhite1F as RestHullWhite1F,
    IRSwap as RestIRSwap,
    Market as RestMarket,
    MarketSpec,
    ProblemDetails,
    QuantContext,
    QuantRestClient,
    QuantRestError,
    ResourceRef,
    ScenarioSet,
)

__all__ = [
    "Engine",
    "PriceResult",
    "BatchRow",
    "GridRow",
    "Portfolio",
    "PAR",
    "TradeSpec",
    "IRSwap",
    "ModelSpec",
    "HullWhite1F",
    "HullWhite2F",
    "Gbm",
    "GbmP",
    "GbmBasket",
    "Market",
    "PricingContext",
    "ExecutionContext",
    "Measure",
    "PV",
    "DV01",
    "ExposureProfile",
    "UnilateralCVA",
    "Greek",
    "greeks",
    "PayoffProduct",
    "ScalarExpr",
    "Predicate",
    "Contract",
    "constant",
    "parameter",
    "fixing",
    "current",
    "add",
    "sub",
    "mul",
    "div",
    "neg",
    "abs",
    "exp",
    "log",
    "pow",
    "minimum",
    "maximum",
    "clamp",
    "average",
    "running_min",
    "running_max",
    "event_time",
    "event_value",
    "discount_factor",
    "fx_conversion",
    "greater",
    "less",
    "greater_equal",
    "less_equal",
    "eq",
    "all_of",
    "any_of",
    "negate",
    "between",
    "event_occurred",
    "before",
    "after",
    "zero",
    "cashflow",
    "give",
    "both",
    "scale",
    "if_",
    "when",
    "trigger",
    "exercise",
    "european_call",
    "european_put",
    "call_leg",
    "put_leg",
    "custom_strategy",
    "irs",
    "fx_forward",
    "QuantRestClient",
    "QuantContext",
    "Context",
    "ResourceRef",
    "ProblemDetails",
    "QuantRestError",
    "MarketSpec",
    "ScenarioSet",
    "RestMarket",
    "RestHullWhite1F",
    "RestIRSwap",
]

# Importar `quantdesk.rest` no debe exigir que la extensión nanobind `engine` esté instalada.
# La fachada nativa se carga bajo demanda cuando alguien solicita uno de sus nombres públicos;
# esto permite probar/usar el SDK REST puro en runners Linux sin compilar el módulo Windows.
_REST_NAMES = {
    "QuantRestClient",
    "QuantContext",
    "Context",
    "ResourceRef",
    "ProblemDetails",
    "QuantRestError",
    "MarketSpec",
    "ScenarioSet",
    "RestMarket",
    "RestHullWhite1F",
    "RestIRSwap",
}
_NATIVE_NAMES = frozenset(__all__) - _REST_NAMES

# Mantén ``from quantdesk import *`` funcional en una instalación REST-only. Cuando la
# extensión existe, los nombres nativos siguen resolviéndose perezosamente mediante
# ``__getattr__``; cuando no existe, la superficie declarada es únicamente la del SDK REST.
if importlib.util.find_spec("engine") is None:
    __all__ = [name for name in __all__ if name in _REST_NAMES]


def __getattr__(name: str):
    if name not in _NATIVE_NAMES:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    from quantdesk import _native_init

    value = getattr(_native_init, name)
    globals()[name] = value
    return value
