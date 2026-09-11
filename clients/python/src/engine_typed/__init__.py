"""`engine_typed`: fachada Python tipada sobre `engine` (PLAN_REAPI.md, propuestas 1-3).

Paquete Python puro, aditivo: traduce objetos `pydantic` (`TradeSpec`/`IRSwap`, y en fases
siguientes `Model`/`Market`/`PricingContext`/`ExecutionContext`/`Measure`) al mismo
`Params`/dict que ya consume `engine.Engine` -- no sustituye la fachada dinámica
(dict/Excel/C ABI), es una fachada más sobre el mismo registry C++ (PLAN_REAPI.md §2, mismo
patrón que el segundo constructor de `MarketSnapshot`).

    import engine, engine_typed as q

    trade = q.IRSwap(
        notional=1_000_000.0, fixed_rate=q.PAR,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    eng = engine.Engine()
    product = eng.create_product(trade.product_type, trade.to_params())
"""

from engine_typed.trade import PAR, IRSwap, TradeSpec

__all__ = ["PAR", "TradeSpec", "IRSwap"]
