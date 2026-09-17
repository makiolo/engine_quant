"""Flujo end-to-end de PLAN_GREEKS.md §9.1: autoria programatica del payoff (`when`/`cashflow`/
`maximum`, PLAN_PRODUCTS.md §7) -> valoracion (`quantdesk.Engine.price`) -> TODAS las Greeks de
esa metrica (`quantdesk.Engine.all_greeks`, §8.5) sin enumerar spot/rate/dividend_yield/
volatility/curva/credito/tiempo a mano. Tambien muestra `quantdesk.greeks` (§8.4) para pedir una
Greek CONCRETA (una combinacion metric/risk_factor) via el mismo `Engine.price` de siempre,
cuando no hace falta el barrido completo.

Nota (PLAN_API_REFACTOR.md Fase 4, "hallazgo no cubierto por ninguna fase anterior"):
`quantdesk.model` solo tipa `HullWhite1F`/`HullWhite2F`/`GbmBasket` -- no existe un
`ModelSpec` para el modelo univariante "GBM" que usa este ejemplo (a diferencia de
`GbmBasket`, que es multi-activo y correlacionado: no es un sustituto válido, cambiaría el
modelo nativo invocado). En vez de recurrir a la fachada dinámica cruda para todo el flujo
de precio/Greeks (lo que dejaría este script sin usar `quantdesk.Engine`, contra lo que pide
esta fase), se define aquí mismo un `ModelSpec` mínimo para "GBM" -- exactamente el mismo
patrón de 2 líneas (`model_type` + `to_params()`) que ya usan `HullWhite1F`/`HullWhite2F` en
`quantdesk/model.py` -- sin tocar el paquete `quantdesk` (fuera de alcance de esta fase).
"""

import sys
from pathlib import Path
from typing import ClassVar

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import quantdesk as q  # noqa: E402
from quantdesk import greeks  # noqa: E402


class Gbm(q.ModelSpec):
    """`ModelSpec` mínimo para `engine::GBM` (movimiento geométrico browniano de un único
    activo) -- ver nota del módulo: `quantdesk.model` no lo tipa todavía."""

    model_type: ClassVar[str] = "GBM"

    s0: float
    r: float
    q: float
    sigma: float
    observable: str

    def to_params(self) -> dict:
        return {"s0": self.s0, "r": self.r, "q": self.q, "sigma": self.sigma, "observable": self.observable}


def main():
    # 1. Autoria programatica (When/Cashflow/Maximum -- PLAN_PRODUCTS.md §7.3): una call europea
    # sobre AAPL, sin plantilla nominal nueva.
    call = q.when(
        1.0,
        q.cashflow("USD", 1_000.0 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100.0, 0)),
    )
    trade = q.PayoffProduct(id="AAPL_CALL_100", contract=call)
    model = Gbm(s0=100.0, r=0.05, q=0.0, sigma=0.2, observable="EQ.SPOT.AAPL")
    market = q.Market(pillars=[1.0], zero_rates=[0.05])

    qeng = q.Engine(backend="cpu", n_paths=200_000, n_steps=1, seed=7, pricing_date=0.0)

    # 2-3. Valoracion: una o varias metricas en una sola llamada (ya existente).
    result = qeng.price(trade, model, market, ["PayoffPriceQ"])
    print(f"PayoffPriceQ = {result.PayoffPriceQ.scalar:,.2f}")

    # 4a. Una Greek CONCRETA via quantdesk.greeks + Engine.price (§8.4): util cuando ya se sabe
    # exactamente que sensibilidad hace falta, sin pagar el barrido completo.
    delta_spec = greeks.delta("PayoffPriceQ", "spot")
    delta_only = qeng.price(trade, model, market, [delta_spec])
    print(f"Delta (spot) = {delta_only.Greek.scalar:,.4f}")

    # 4b. TODAS las Greeks de esa metrica (§8.5), sin enumerar spot/rate/dividend_yield/
    # volatility/curva/credito/tiempo a mano.
    report = qeng.all_greeks(trade, "PayoffPriceQ", model, market)
    print("\nTodas las Greeks de PayoffPriceQ:")
    for g in report.greeks:
        print(f"  {g.risk_factor:<20} = {g.value:>14,.4f}  (metodo={g.method_used}, medida={g.measure})")
    if report.skipped:
        print("\nOmitidas (candidatos que no aplican a esta metrica/modelo):")
        for reason in report.skipped:
            print(f"  {reason}")

    # include_second_order=True añade la Gamma pura de cada parametro de modelo (§8.5 punto 5).
    report_with_gamma = qeng.all_greeks(trade, "PayoffPriceQ", model, market, include_second_order=True)
    gammas = [g for g in report_with_gamma.greeks if g.order == 2]
    print("\nGamma pura de cada parametro de modelo (include_second_order=True):")
    for g in gammas:
        print(f"  {g.risk_factor:<20} = {g.value:>14,.6f}")


if __name__ == "__main__":
    main()
