"""Flujo completo de price_batch/price_many/price_grid (PLAN.md §7.19) desde Python usando
`quantdesk` (PLAN_API_REFACTOR.md): tres niveles de la API de cálculo por lotes, cada uno
construido sobre el anterior -- ver price_flow.py (PLAN.md §7.15) para el equivalente de un
solo trade que esta API extiende.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import quantdesk as q  # noqa: E402


def main():
    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    model_2f = q.HullWhite2F(a=0.1, b=0.2, sigma=0.01, eta=0.012, rho=-0.7, r0=0.03)
    market = q.Market(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    market_stressed = q.Market(pillars=[1.0], zero_rates=[0.05], hazard_rate=0.05, recovery_rate=0.3)

    qeng = q.Engine(backend="auto", n_paths=5000, n_steps=208, seed=7)

    # price_batch: lote homogéneo -- mismo calendario, cada trade con su propio notional/
    # fixed_rate explícito (sin "a la par": el lote no lo soporta).
    payment_times = [1.0, 2.0, 3.0, 4.0, 5.0]
    accruals = [1.0] * 5
    trade_a = q.IRSwap(notional=1_000_000.0, fixed_rate=0.02, payment_times=payment_times, accruals=accruals)
    trade_b = q.IRSwap(notional=2_500_000.0, fixed_rate=0.015, payment_times=payment_times, accruals=accruals)
    trade_c = q.IRSwap(notional=500_000.0, fixed_rate=0.025, payment_times=payment_times, accruals=accruals)

    print("--- price_batch (lote homogéneo) ---")
    batch = qeng.price_batch([trade_a, trade_b, trade_c], model, market, ["PV", "UnilateralCVA"])
    for row in batch:
        print(f"  trade_index={row.trade_index}  PV={row.measures.PV.scalar:>12,.2f}  CVA={row.measures.UnilateralCVA.scalar:>10,.2f}")

    # price_many: lote heterogéneo -- un trade a 3 años intercalado entre los de 5 años. Se
    # agrupan internamente por calendario y el resultado vuelve en el orden de entrada.
    trade_3y = q.IRSwap(notional=2_000_000.0, fixed_rate=0.018, payment_times=[1.0, 2.0, 3.0], accruals=[1.0] * 3)

    print("--- price_many (lote heterogéneo: 5y, 3y, 5y intercalados) ---")
    many = qeng.price_many([trade_a, trade_3y, trade_b], model, market, ["PV"])
    for row in many:
        print(f"  trade_index={row.trade_index}  PV={row.measures.PV.scalar:>12,.2f}")

    # price_grid: Trades x Models x Markets -- PricingContext/ExecutionContext compartidos
    # (fijados una vez en el constructor de Engine).
    print("--- price_grid (2 trades x 2 modelos x 2 mercados) ---")
    grid = qeng.price_grid(
        [trade_a, trade_b], [model, model_2f], [market, market_stressed], ["PV", "UnilateralCVA"]
    )
    for cell in grid:
        print(
            f"  trade={cell.trade_index} model={cell.model_index} market={cell.market_index}  "
            f"PV={cell.measures.PV.scalar:>12,.2f}  CVA={cell.measures.UnilateralCVA.scalar:>10,.2f}"
        )


if __name__ == "__main__":
    main()
