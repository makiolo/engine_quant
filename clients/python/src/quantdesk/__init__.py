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

import engine as _native

from quantdesk import greeks
from quantdesk.context import ExecutionContext, PricingContext
from quantdesk.engine import BatchRow, Engine, GridRow, PriceResult
from quantdesk.greeks import Greek
from quantdesk.market import Market
from quantdesk.measure import PV, DV01, ExposureProfile, Measure, UnilateralCVA
from quantdesk.model import GbmBasket, HullWhite1F, HullWhite2F, ModelSpec
from quantdesk.payoff import (
    Contract,
    PayoffProduct,
    Predicate,
    ScalarExpr,
    abs,
    add,
    after,
    all_of,
    any_of,
    average,
    before,
    between,
    both,
    call_leg,
    cashflow,
    clamp,
    constant,
    current,
    custom_strategy,
    discount_factor,
    div,
    eq,
    european_call,
    european_put,
    event_occurred,
    event_time,
    event_value,
    exercise,
    exp,
    fixing,
    fx_conversion,
    fx_forward,
    give,
    greater,
    greater_equal,
    if_,
    irs,
    less,
    less_equal,
    log,
    maximum,
    minimum,
    mul,
    neg,
    negate,
    parameter,
    pow,
    put_leg,
    running_max,
    running_min,
    scale,
    sub,
    trigger,
    when,
    zero,
)
from quantdesk.trade import PAR, IRSwap, TradeSpec

# `Portfolio` (PLAN_API_REFACTOR.md §3.2): ya "suficientemente pythónico" según su propio
# comentario en `engine_py_ext.cpp` (cuatro métodos, sin dict de por medio) -- reexport directo
# desde `engine.Portfolio`, sin envoltorio adicional (mismo criterio que ya aplica el binding
# nativo hoy). No requiere que el módulo nativo esté importable a nivel de PAQUETE quantdesk
# (import engine as _native, como ya hace quantdesk.engine) -- se resuelve al mismo módulo
# nanobind compilado, nunca a quantdesk.engine.
Portfolio = _native.Portfolio

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
]
