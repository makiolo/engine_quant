# Future plans

- PLAN_PORTFOLIO.md: Design first class objects Deal/Trade, Portfolio ...
- PLAN_PIPELINE.md: Design main pipeline. Simulation, Pnl, Greeks ...
- PLAN_HEDGE.md: From one portfolio (current deals + previous hedges) calculate new alternative Portfolio for Hedge Gamma/Theta/Vega with options, and Delta with Forwards/Equity/Spot Market.
- PLAN_PRODUCTS_TEMPLATIZED.md: Use Jinja for products and solve with deal data.

# New Metrics:

q.ExpectedPnL(horizon="1D")
q.ProbabilityOfProfit(horizon="1D")
q.ExpectedReturn(horizon="1D")
q.FXDelta()
q.FXGamma()
q.DomesticPV01()
q.ForeignPV01()
q.BasisDelta()
q.Carry()
q.RollDown()
q.PnL()
q.PV01(curve="USD.OIS")
q.PV01(curve="EUR.OIS")
q.BucketedPV01(curve="USD.OIS")
q.BucketedPV01(curve="EUR.OIS")
q.BasisDelta(curve="EURUSD.BASIS")

# New models:

q.FXBlackScholes(
    pair="EURUSD",
    volatility=...
)

q.CrossCurrencyModel(
    domestic=q.HullWhite1F(...),
    foreign=q.HullWhite1F(...),
    fx=q.LognormalFX(...),
    correlation=...
)

# Separate PricingContext in two:

```
valuation = q.ValuationConfig(
    as_of="2026-09-11",
)

simulation = q.SimulationConfig(
    paths=50_000,
    grid=q.TimeGrid.monthly("10Y"),
    rng=q.Sobol(seed=7),
)
```
