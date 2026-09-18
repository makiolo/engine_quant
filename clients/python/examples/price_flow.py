"""Flujo completo de ENGINE.PRICE (PLAN.md §7.15) desde Python usando `quantdesk`
(PLAN_API_REFACTOR.md): `Trade`/`Model`/`Market` construidos como objetos `pydantic`
(`qd.IRSwap`/`qd.HullWhite1F`/`qd.Market`), y `qd.Engine(...)` fija `PricingContext`/
`ExecutionContext` una sola vez en el constructor -- luego `engine.price(...)` calcula un
lote de medidas de una sola vez, sin traducir a mano cada objeto tipado a su equivalente
nativo (`eng.create_product`/`create_model`/`MarketSnapshot`/`PricingContext`/
`ExecutionContext`), que es lo que hacía el flujo anterior sobre `engine_typed`.
`quantdesk` es la fachada recomendada desde Python -- la fachada dinámica (dict crudo,
módulo `engine`) sigue existiendo y sigue soportada (Excel/C ABI la usan tal cual, y
`quantdesk.Engine` la sigue usando por dentro), pero para Python `quantdesk` da
validación real y evita errores de "clave mal escrita" que un dict no detecta hasta
tiempo de ejecución. Para ilustrar justo esa relación, este script usa el módulo `engine`
crudo (fachada dinámica) solo para el detalle de bajo nivel que `quantdesk.Engine` no
expone -- el backend "auto" ya resuelto ("cpu"/"gpu") -- y `quantdesk` para todo lo demás.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402  -- fachada dinámica cruda, solo para el backend ya resuelto
import quantdesk as qd  # noqa: E402


def main():
    trade = qd.IRSwap(
        notional=1_000_000.0,
        fixed_rate=0.02,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0],
        accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    model = qd.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    market = qd.Market(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)

    # "auto" se resuelve una vez, al construir el motor, a "gpu" si este build tiene soporte
    # GPU (engine.is_gpu_backend_available()); quantdesk.Engine no expone el resultado de esa
    # resolución directamente, así que se consulta con la fachada dinámica cruda.
    resolved_backend = engine.ExecutionContext({"backend": "auto", "precision": "FP64"}).backend
    print(f"Backend resuelto: {resolved_backend}")

    qeng = qd.Engine(backend="auto", n_paths=5000, n_steps=208, seed=7)

    result = qeng.price(trade, model, market, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"])

    print(f"PV             = {result.PV.scalar:,.2f}")
    print(f"DV01           = {result.DV01.scalar:,.2f}")
    print(f"UnilateralCVA  = {result.UnilateralCVA.scalar:,.2f}")
    print("ExpectedExposure / PFE95 por fecha de reseteo:")
    ee, pfe = result.ExpectedExposure, result.PFE95
    for t, ee_i, pfe_i in zip(ee.times, ee.primary, pfe.primary):
        print(f"  t={t:.0f}: EE={ee_i:,.2f}  PFE95={pfe_i:,.2f}")

    # Swap "a la par" (PLAN_REAPI.md §3.2, propuesta 2): construcción a mercado explícita vía
    # IRSwap.par(...) -- omitir fixed_rate directamente sería un ValidationError, no PAR.
    par_trade = qd.IRSwap.par(
        notional=1_000_000.0,
        payment_times=[1.0, 2.0, 3.0, 4.0, 5.0],
        accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
    )
    par_pv = qeng.price(par_trade, model, market, ["PV"]).PV.scalar
    print(f"PV (swap par)  = {par_pv:,.6f}")


if __name__ == "__main__":
    main()
