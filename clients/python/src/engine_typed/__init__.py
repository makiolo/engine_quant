"""`engine_typed`: fachada Python tipada sobre `engine` (PLAN_REAPI.md, propuestas 1-3).

Paquete Python puro, aditivo: traduce objetos `pydantic` (`TradeSpec`/`IRSwap`, `Model`/
`HullWhite1F`/`HullWhite2F`, `Market`, `PricingContext`, `ExecutionContext`, y en fases
siguientes `Measure`) al mismo `Params`/dict que ya consume `engine.Engine` -- no sustituye la
fachada dinámica (dict/Excel/C ABI), es una fachada más sobre el mismo registry C++
(PLAN_REAPI.md §2, mismo patrón que el segundo constructor de `MarketSnapshot`).

    import engine, engine_typed as q

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

from engine_typed.context import ExecutionContext, PricingContext
from engine_typed.market import Market
from engine_typed.measure import PV, DV01, ExposureProfile, Measure, UnilateralCVA
from engine_typed.model import HullWhite1F, HullWhite2F, ModelSpec
from engine_typed.trade import PAR, IRSwap, TradeSpec

__all__ = [
    "PAR",
    "TradeSpec",
    "IRSwap",
    "ModelSpec",
    "HullWhite1F",
    "HullWhite2F",
    "Market",
    "PricingContext",
    "ExecutionContext",
    "Measure",
    "PV",
    "DV01",
    "ExposureProfile",
    "UnilateralCVA",
]
