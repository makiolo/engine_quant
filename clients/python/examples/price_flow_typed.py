"""Equivalente completo de `price_flow.py` usando solo `engine_typed` (PLAN_REAPI.md §6 Fase 2):
cero dicts crudos -- Trade/Model/Market/PricingContext/ExecutionContext tipados, `.to_params()`
alimenta el mismo `Engine.price(...)` de siempre.
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
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    # Curva multi-pillar real (PLAN_REAPI.md §6 Fase 4/5): PV/DV01 descuentan por esta curva,
    # no por el modelo -- una curva de un solo pillar "funciona" pero da una réplica plana poco
    # realista para un swap a 5 años. hazard_rate/recovery_rate (usados solo por UnilateralCVA)
    # no dependen de la forma de la curva.
    market = q.Market(
        pillars=[1.0, 2.0, 3.0, 4.0, 5.0], zero_rates=[0.018, 0.019, 0.020, 0.0205, 0.021],
        hazard_rate=0.02, recovery_rate=0.4,
    )
    pricing = q.PricingContext(n_paths=5000, n_steps=208, seed=7)
    execution = q.ExecutionContext(backend="auto")

    product = eng.create_product(trade.product_type, trade.to_params())
    eng_model = eng.create_model(model.model_type, model.to_params())
    eng_market = engine.MarketSnapshot(**market.to_params())
    eng_pricing = engine.PricingContext(pricing.to_params())
    eng_execution = engine.ExecutionContext(execution.to_params())

    print(f"Backend resuelto: {eng_execution.backend}")

    # Medidas tipadas (PLAN_REAPI.md §6 Fase 3, propuesta 3): q.DV01(bump=...).to_spec() es una
    # tupla (nombre, params) -- conviven con strings "pelados" en la misma llamada.
    measures = [
        q.PV().to_spec(),
        q.DV01().to_spec(),
        "ExpectedExposure",
        "PFE95",
        q.UnilateralCVA().to_spec(),
    ]
    result = eng.price(product, measures, eng_model, eng_market, eng_pricing, eng_execution)

    print(f"PV             = {result['PV'].scalar:,.2f}")
    print(f"DV01 (1bp)     = {result['DV01'].scalar:,.2f}")
    dv01_2bp = eng.price(product, [q.DV01(bump=0.0002).to_spec()], eng_model, eng_market, eng_pricing, eng_execution)
    print(f"DV01 (2bp)     = {dv01_2bp['DV01'].scalar:,.2f}")

    # DV01(bucketed=True) (PLAN_REAPI.md §6 Fase 5): un delta por pillar en vez de un escalar --
    # su suma coincide con el DV01 "parcial" de arriba (bump paralelo).
    bucketed = eng.price(product, [q.DV01(bucketed=True).to_spec()], eng_model, eng_market, eng_pricing, eng_execution)["DV01"]
    print("DV01 bucketed (por pillar):")
    for t, delta in zip(bucketed.times, bucketed.primary):
        print(f"  pillar={t:.0f}y: {delta:,.2f}")
    print(f"  suma            = {sum(bucketed.primary):,.2f}")

    print(f"UnilateralCVA  = {result['UnilateralCVA'].scalar:,.2f}")
    print("ExpectedExposure / PFE95 por fecha de reseteo:")
    ee, pfe = result["ExpectedExposure"], result["PFE95"]
    for t, ee_i, pfe_i in zip(ee.times, ee.primary, pfe.primary):
        print(f"  t={t:.0f}: EE={ee_i:,.2f}  PFE95={pfe_i:,.2f}")


if __name__ == "__main__":
    main()
