"""Flujo completo de price_batch/price_many/price_grid (PLAN.md §7.19) desde Python usando
`engine_typed` (PLAN_REAPI.md §6): tres niveles de la API de cálculo por lotes, cada uno
construido sobre el anterior -- ver price_flow.py (PLAN.md §7.15) para el equivalente de un
solo trade que esta API extiende.
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

    model = q.HullWhite1F(a=0.1, b=0.03, sigma=0.01, r0=0.02)
    model_2f = q.HullWhite2F(a=0.1, b=0.2, sigma=0.01, eta=0.012, rho=-0.7, r0=0.03)
    market = q.Market(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    market_stressed = q.Market(pillars=[1.0], zero_rates=[0.05], hazard_rate=0.05, recovery_rate=0.3)
    pricing = q.PricingContext(n_paths=5000, n_steps=208, seed=7)
    execution = q.ExecutionContext(backend="auto")

    eng_model = eng.create_model(model.model_type, model.to_params())
    eng_model_2f = eng.create_model(model_2f.model_type, model_2f.to_params())
    eng_market = engine.MarketSnapshot(**market.to_params())
    eng_market_stressed = engine.MarketSnapshot(**market_stressed.to_params())
    eng_pricing = engine.PricingContext(pricing.to_params())
    eng_execution = engine.ExecutionContext(execution.to_params())

    # price_batch: lote homogéneo -- mismo calendario, cada trade con su propio notional/
    # fixed_rate explícito (sin "a la par": el lote no lo soporta).
    payment_times = [1.0, 2.0, 3.0, 4.0, 5.0]
    accruals = [1.0] * 5
    trade_a = q.IRSwap(notional=1_000_000.0, fixed_rate=0.02, payment_times=payment_times, accruals=accruals)
    trade_b = q.IRSwap(notional=2_500_000.0, fixed_rate=0.015, payment_times=payment_times, accruals=accruals)
    trade_c = q.IRSwap(notional=500_000.0, fixed_rate=0.025, payment_times=payment_times, accruals=accruals)

    eng_trade_a = eng.create_product(trade_a.product_type, trade_a.to_params())
    eng_trade_b = eng.create_product(trade_b.product_type, trade_b.to_params())
    eng_trade_c = eng.create_product(trade_c.product_type, trade_c.to_params())

    print("--- price_batch (lote homogéneo) ---")
    batch = eng.price_batch(
        [eng_trade_a, eng_trade_b, eng_trade_c], ["PV", "UnilateralCVA"],
        eng_model, eng_market, eng_pricing, eng_execution,
    )
    for row in batch:
        print(f"  trade_index={row.trade_index}  PV={row.measures['PV'].scalar:>12,.2f}  CVA={row.measures['UnilateralCVA'].scalar:>10,.2f}")

    # price_many: lote heterogéneo -- un trade a 3 años intercalado entre los de 5 años. Se
    # agrupan internamente por calendario y el resultado vuelve en el orden de entrada.
    trade_3y = q.IRSwap(notional=2_000_000.0, fixed_rate=0.018, payment_times=[1.0, 2.0, 3.0], accruals=[1.0] * 3)
    eng_trade_3y = eng.create_product(trade_3y.product_type, trade_3y.to_params())

    print("--- price_many (lote heterogéneo: 5y, 3y, 5y intercalados) ---")
    many = eng.price_many(
        [eng_trade_a, eng_trade_3y, eng_trade_b], ["PV"], eng_model, eng_market, eng_pricing, eng_execution
    )
    for row in many:
        print(f"  trade_index={row.trade_index}  PV={row.measures['PV'].scalar:>12,.2f}")

    # price_grid: Trades x Models x Markets -- PricingContext/ExecutionContext compartidos.
    print("--- price_grid (2 trades x 2 modelos x 2 mercados) ---")
    grid = eng.price_grid(
        [eng_trade_a, eng_trade_b], ["PV", "UnilateralCVA"],
        [eng_model, eng_model_2f], [eng_market, eng_market_stressed],
        eng_pricing, eng_execution,
    )
    for cell in grid:
        print(
            f"  trade={cell.trade_index} model={cell.model_index} market={cell.market_index}  "
            f"PV={cell.measures['PV'].scalar:>12,.2f}  CVA={cell.measures['UnilateralCVA'].scalar:>10,.2f}"
        )


if __name__ == "__main__":
    main()
