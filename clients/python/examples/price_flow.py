"""Flujo completo de ENGINE.PRICE (PLAN.md §7.15) desde Python usando `engine_typed`
(PLAN_REAPI.md §6): `Trade`/`Model`/`Market`/`PricingContext`/`ExecutionContext` construidos
como objetos `pydantic` (`q.IRSwap`/`q.HullWhite1F`/`q.Market`/`q.PricingContext`/
`q.ExecutionContext`), traducidos a `Params`/dict vía `.to_params()` sin tocar el core, luego
`Engine.price(...)` calculando un lote de medidas de una sola vez. `engine_typed` es la
fachada recomendada desde Python -- la fachada dinámica (dict crudo) sigue existiendo y sigue
soportada (Excel/C ABI la usan tal cual), pero para Python `engine_typed` da validación real
y evita errores de "clave mal escrita" que un dict no detecta hasta tiempo de ejecución.
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
    market = q.Market(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    pricing = q.PricingContext(n_paths=5000, n_steps=208, seed=7)
    # "auto" se resuelve una vez, aquí, a "gpu" si este build tiene soporte GPU
    # (engine.is_gpu_backend_available()).
    execution = q.ExecutionContext(backend="auto")

    product = eng.create_product(trade.product_type, trade.to_params())
    eng_model = eng.create_model(model.model_type, model.to_params())
    eng_market = engine.MarketSnapshot(**market.to_params())
    eng_pricing = engine.PricingContext(pricing.to_params())
    eng_execution = engine.ExecutionContext(execution.to_params())

    print(f"Backend resuelto: {eng_execution.backend}")

    result = eng.price(
        product, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"],
        eng_model, eng_market, eng_pricing, eng_execution,
    )

    print(f"PV             = {result['PV'].scalar:,.2f}")
    print(f"DV01           = {result['DV01'].scalar:,.2f}")
    print(f"UnilateralCVA  = {result['UnilateralCVA'].scalar:,.2f}")
    print("ExpectedExposure / PFE95 por fecha de reseteo:")
    ee, pfe = result["ExpectedExposure"], result["PFE95"]
    for t, ee_i, pfe_i in zip(ee.times, ee.primary, pfe.primary):
        print(f"  t={t:.0f}: EE={ee_i:,.2f}  PFE95={pfe_i:,.2f}")

    # Swap "a la par" (PLAN_REAPI.md §3.2, propuesta 2): construcción a mercado explícita vía
    # IRSwap.par(...) -- omitir fixed_rate directamente sería un ValidationError, no PAR.
    par_trade = q.IRSwap.par(
        notional=1_000_000.0,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0],
        accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    par_product = eng.create_product(par_trade.product_type, par_trade.to_params())
    par_pv = eng.price(par_product, ["PV"], eng_model, eng_market, eng_pricing, eng_execution)["PV"].scalar
    print(f"PV (swap par)  = {par_pv:,.6f}")


if __name__ == "__main__":
    main()
