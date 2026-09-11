"""Equivalente de `calc_flow.py` usando `engine_typed.IRSwap` en vez del dict dinámico
(PLAN_REAPI.md §6 Fase 1): mismo `Engine.calc(...)`, la única diferencia es cómo se construye
el `product` -- `trade.to_params()` alimenta `eng.create_product(...)` sin tocar el core.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402
import engine_typed as q  # noqa: E402


def main():
    eng = engine.Engine()

    trade = q.IRSwap(
        notional=1_000_000.0,
        fixed_rate=0.02,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0],
        accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    product = eng.create_product(trade.product_type, trade.to_params())

    model = eng.create_model("HullWhite1F", {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02})
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 5000.0, "n_steps": 208.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "auto", "precision": "FP64"})

    print(f"Backend resuelto: {execution.backend}")

    result = eng.calc(product, ["PV", "DV01", "UnilateralCVA"], model, market, pricing, execution)
    print(f"PV             = {result['PV'].scalar:,.2f}")
    print(f"DV01           = {result['DV01'].scalar:,.2f}")
    print(f"UnilateralCVA  = {result['UnilateralCVA'].scalar:,.2f}")

    # q.IRSwap.par(...) -- construcción a mercado explícita (propuesta 2, PLAN_REAPI.md §3.2).
    par_trade = q.IRSwap.par(
        notional=1_000_000.0,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0],
        accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    par_product = eng.create_product(par_trade.product_type, par_trade.to_params())
    par_pv = eng.calc(par_product, ["PV"], model, market, pricing, execution)["PV"].scalar
    print(f"PV (swap par)  = {par_pv:,.6f}")


if __name__ == "__main__":
    main()
