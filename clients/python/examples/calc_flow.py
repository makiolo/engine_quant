"""Flujo completo de ENGINE.CALC (PLAN.md §7.15) desde Python: Trade + Model + Market +
PricingContext + ExecutionContext -> Engine.calc(...) con un lote de medidas en una sola
llamada. Sustituye al antiguo ejemplo de selección de backend (`backend_selection.py`,
PLAN.md §7.12), que trataba un mecanismo -- el backend global de proceso -- eliminado en esta
fase en favor de `ExecutionContext`.
"""

import sys

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def main():
    eng = engine.Engine()

    trade = eng.create_product(
        "IRSwap",
        {
            "notional": 1_000_000.0,
            "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
            "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
        },
    )
    model = eng.create_model("HullWhite1F", {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02})
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 5000.0, "n_steps": 208.0, "seed": 7.0})
    # "auto" se resuelve una vez, aquí, a "gpu" si este build tiene soporte GPU (engine.is_gpu_backend_available()).
    execution = engine.ExecutionContext({"backend": "auto", "precision": "FP64"})

    print(f"Backend resuelto: {execution.backend}")

    result = eng.calc(trade, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"], model, market, pricing, execution)

    print(f"PV             = {result['PV'].scalar:,.2f}")
    print(f"DV01           = {result['DV01'].scalar:,.2f}")
    print(f"UnilateralCVA  = {result['UnilateralCVA'].scalar:,.2f}")
    print("ExpectedExposure / PFE95 por fecha de reseteo:")
    ee, pfe = result["ExpectedExposure"], result["PFE95"]
    for t, ee_i, pfe_i in zip(ee.times, ee.primary, pfe.primary):
        print(f"  t={t:.0f}: EE={ee_i:,.2f}  PFE95={pfe_i:,.2f}")


if __name__ == "__main__":
    main()
