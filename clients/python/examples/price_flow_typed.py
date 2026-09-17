"""Showcase de medidas tipadas con configuración real sobre `quantdesk`
(PLAN_REAPI.md §6 Fases 3-5) -- construida sobre el mismo patrón que `price_flow.py`
(`Trade`/`Model`/`Market` tipados, `quantdesk.Engine` resuelve `PricingContext`/
`ExecutionContext` una sola vez en el constructor), pero centrada en lo que
`price_flow.py` no cubre: `q.DV01(bump=...)`/`q.DV01(bucketed=True)` -- medidas con
`Params` real, ver `.to_spec()` -- y una curva de mercado multi-pillar real (no de un solo
punto) para que la forma de la curva importe de verdad en PV/DV01.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402  -- fachada dinámica cruda, solo para el backend ya resuelto
import quantdesk as q  # noqa: E402


def main():
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

    # "auto" se resuelve una vez, al construir el motor, a "gpu" si este build tiene soporte
    # GPU -- ver price_flow.py para el mismo patrón explicado.
    resolved_backend = engine.ExecutionContext({"backend": "auto", "precision": "FP64"}).backend
    print(f"Backend resuelto: {resolved_backend}")

    qeng = q.Engine(backend="auto", n_paths=5000, n_steps=208, seed=7)

    # Medidas tipadas (PLAN_REAPI.md §6 Fase 3, propuesta 3): conviven con strings "pelados"
    # en la misma llamada -- `quantdesk.Engine.price` traduce `q.PV()`/`q.DV01()` a su spec
    # internamente, igual que hacía `eng.price(..., [q.PV().to_spec(), ...])` antes.
    measures = [q.PV(), q.DV01(), "ExpectedExposure", "PFE95", q.UnilateralCVA()]
    result = qeng.price(trade, model, market, measures)

    print(f"PV             = {result.PV.scalar:,.2f}")
    print(f"DV01 (1bp)     = {result.DV01.scalar:,.2f}")
    dv01_2bp = qeng.price(trade, model, market, [q.DV01(bump=0.0002)])
    print(f"DV01 (2bp)     = {dv01_2bp.DV01.scalar:,.2f}")

    # DV01(bucketed=True) (PLAN_REAPI.md §6 Fase 5): un delta por pillar en vez de un escalar --
    # su suma coincide con el DV01 "parcial" de arriba (bump paralelo).
    bucketed = qeng.price(trade, model, market, [q.DV01(bucketed=True)]).DV01
    print("DV01 bucketed (por pillar):")
    for t, delta in zip(bucketed.times, bucketed.primary):
        print(f"  pillar={t:.0f}y: {delta:,.2f}")
    print(f"  suma            = {sum(bucketed.primary):,.2f}")

    print(f"UnilateralCVA  = {result.UnilateralCVA.scalar:,.2f}")
    print("ExpectedExposure / PFE95 por fecha de reseteo:")
    ee, pfe = result.ExpectedExposure, result.PFE95
    for t, ee_i, pfe_i in zip(ee.times, ee.primary, pfe.primary):
        print(f"  t={t:.0f}: EE={ee_i:,.2f}  PFE95={pfe_i:,.2f}")


if __name__ == "__main__":
    main()
