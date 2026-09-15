"""Flujo end-to-end de PLAN_GREEKS.md §9.1: autoria programatica del payoff (`when`/`cashflow`/
`maximum`, PLAN_PRODUCTS.md §7) -> creacion de producto/modelo/mercado -> valoracion
(`Engine.price`, ya existente) -> TODAS las Greeks de esa metrica (`Engine.all_greeks`, §8.5)
sin enumerar spot/rate/dividend_yield/volatility/curva/credito/tiempo a mano. Tambien muestra
`engine_typed.greeks` (§8.4) para pedir una Greek CONCRETA (una combinacion metric/risk_factor)
via el mismo `Engine.price` de siempre, cuando no hace falta el barrido completo.
"""

import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import engine  # noqa: E402
import engine_typed as q  # noqa: E402
from engine_typed import greeks  # noqa: E402


def main():
    # 1. Autoria programatica (When/Cashflow/Maximum -- PLAN_PRODUCTS.md §7.3): una call europea
    # sobre AAPL, sin plantilla nominal nueva.
    call = q.when(
        1.0,
        q.cashflow("USD", 1_000.0 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100.0, 0)),
    )
    trade = q.PayoffProduct(id="AAPL_CALL_100", contract=call)

    eng = engine.Engine()
    product = eng.create_product(trade.product_type, trade.to_params())
    model = eng.create_model(
        "GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"}
    )
    market = engine.MarketSnapshot(pillars=[1.0], zero_rates=[0.05])
    pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 200_000.0, "n_steps": 1.0, "seed": 7.0})
    execution = engine.ExecutionContext({"backend": "cpu"})

    # 2-3. Valoracion: una o varias metricas en una sola llamada (ya existente).
    result = eng.price(product, ["PayoffPriceQ"], model, market, pricing, execution)
    print(f"PayoffPriceQ = {result['PayoffPriceQ'].scalar:,.2f}")

    # 4a. Una Greek CONCRETA via engine_typed.greeks + Engine.price (§8.4): util cuando ya se
    # sabe exactamente que sensibilidad hace falta, sin pagar el barrido completo.
    delta_spec = greeks.delta("PayoffPriceQ", "spot").to_spec()
    delta_only = eng.price(product, [delta_spec], model, market, pricing, execution)
    print(f"Delta (spot) = {delta_only['Greek'].scalar:,.4f}")

    # 4b. TODAS las Greeks de esa metrica (§8.5), sin enumerar spot/rate/dividend_yield/
    # volatility/curva/credito/tiempo a mano.
    report = eng.all_greeks(product, "PayoffPriceQ", model, market, pricing, execution)
    print("\nTodas las Greeks de PayoffPriceQ:")
    for g in report.greeks:
        print(f"  {g.risk_factor:<20} = {g.value:>14,.4f}  (metodo={g.method_used}, medida={g.measure})")
    if report.skipped:
        print("\nOmitidas (candidatos que no aplican a esta metrica/modelo):")
        for reason in report.skipped:
            print(f"  {reason}")

    # include_second_order=True añade la Gamma pura de cada parametro de modelo (§8.5 punto 5).
    report_with_gamma = eng.all_greeks(
        product, "PayoffPriceQ", model, market, pricing, execution, include_second_order=True
    )
    gammas = [g for g in report_with_gamma.greeks if g.order == 2]
    print("\nGamma pura de cada parametro de modelo (include_second_order=True):")
    for g in gammas:
        print(f"  {g.risk_factor:<20} = {g.value:>14,.6f}")


if __name__ == "__main__":
    main()
