"""Flujo completo de calc_batch/calc_many/calc_grid (PLAN.md §7.19) desde Python: tres niveles
de la API de cálculo por lotes, cada uno construido sobre el anterior -- ver calc_flow.py
(PLAN.md §7.15) para el equivalente de un solo trade que esta API extiende.
"""

import sys

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def main():
    eng = engine.Engine()

    model = eng.create_model("HullWhite1F", {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02})
    model_2f = eng.create_model(
        "HullWhite2F", {"a": 0.1, "b": 0.2, "sigma": 0.01, "eta": 0.012, "rho": -0.7, "r0": 0.03}
    )
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.02], hazard_rate=0.02, recovery_rate=0.4)
    market_stressed = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.05], hazard_rate=0.05, recovery_rate=0.3)
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 5000.0, "n_steps": 208.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "auto", "precision": "FP64"})

    # calc_batch: lote homogéneo -- mismo calendario, cada trade con su propio notional/
    # fixed_rate explícito (sin "a la par": el lote no lo soporta).
    trade_a = eng.create_product(
        "IRSwap",
        {"notional": 1_000_000.0, "fixed_rate": 0.02, "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0], "accruals": [1.0] * 5},
    )
    trade_b = eng.create_product(
        "IRSwap",
        {"notional": 2_500_000.0, "fixed_rate": 0.015, "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0], "accruals": [1.0] * 5},
    )
    trade_c = eng.create_product(
        "IRSwap",
        {"notional": 500_000.0, "fixed_rate": 0.025, "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0], "accruals": [1.0] * 5},
    )

    print("--- calc_batch (lote homogéneo) ---")
    batch = eng.calc_batch([trade_a, trade_b, trade_c], ["PV", "UnilateralCVA"], model, market, pricing, execution)
    for row in batch:
        print(f"  trade_index={row.trade_index}  PV={row.measures['PV'].scalar:>12,.2f}  CVA={row.measures['UnilateralCVA'].scalar:>10,.2f}")

    # calc_many: lote heterogéneo -- un trade a 3 años intercalado entre los de 5 años. Se
    # agrupan internamente por calendario y el resultado vuelve en el orden de entrada.
    trade_3y = eng.create_product(
        "IRSwap", {"notional": 2_000_000.0, "fixed_rate": 0.018, "payment_times": [1.0, 2.0, 3.0], "accruals": [1.0] * 3}
    )

    print("--- calc_many (lote heterogéneo: 5y, 3y, 5y intercalados) ---")
    many = eng.calc_many([trade_a, trade_3y, trade_b], ["PV"], model, market, pricing, execution)
    for row in many:
        print(f"  trade_index={row.trade_index}  PV={row.measures['PV'].scalar:>12,.2f}")

    # calc_grid: Trades x Models x Markets -- PricingContext/ExecutionContext compartidos.
    print("--- calc_grid (2 trades x 2 modelos x 2 mercados) ---")
    grid = eng.calc_grid(
        [trade_a, trade_b], ["PV", "UnilateralCVA"], [model, model_2f], [market, market_stressed], pricing, execution
    )
    for cell in grid:
        print(
            f"  trade={cell.trade_index} model={cell.model_index} market={cell.market_index}  "
            f"PV={cell.measures['PV'].scalar:>12,.2f}  CVA={cell.measures['UnilateralCVA'].scalar:>10,.2f}"
        )


if __name__ == "__main__":
    main()
