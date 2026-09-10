# Engine Quant

<pre>
  _______  _______  _______  _______  _______  _______  _______
 |   _   ||   _   ||   _   ||   _   ||   _   ||   _   ||   _   |
 |  | |  ||  | |  ||  | |  ||  | |  ||  | |  ||  | |  ||  | |  |
 |  |_|  ||  |_|  ||  |_|  ||  |_|  ||  |_|  ||  |_|  ||  |_|  |
 |       ||       ||       ||       ||       ||       ||       |
 |  _   | |  _   ||  _   ||  _   ||  _   ||  _   ||  _   ||  _   |
 | | |  || | |  || | |  || | |  || | |  || | |  || | |  || | |  |
 | |_|  || |_|  || |_|  || |_|  || |_|  || |_|  || |_|  || |_|  |
 |_______||_______||_______||_______||_______||_______||_______|
 </pre>

Engine Quant is a research-oriented XVA engine for pricing and risk analytics across interest-rate products, exposure curves, and counterparty credit adjustments.

The core idea is simple:

- one model, one product, one measure registry
- the same semantics everywhere: Python, Excel, and C++
- a Rust numerical core, a C++ orchestration layer, and thin clients on top

This project is intentionally designed around the idea that a new product or model should be registered once and then become available to all frontends without re-implementing logic in each client.

## Why this project exists

Most quant libraries focus on either:

- a single language, or
- a product-specific calculator, or
- a pricing engine that is hard to extend

Engine Quant tries to be different.

It aims to unify three layers that are often separated in financial software:

1. numerical engine: fast Monte Carlo / tensor-based pricing kernels
2. domain layer: models, products, measures, and XVA registries
3. interface layer: Python notebooks, Excel, and future stable C ABI integrations

The deliberate goal is not just to “price one swap.”
The goal is to create a reusable XVA platform where the business logic is centralized and clients stay thin.

## Architecture at a glance

```text
┌──────────────────────────────────────────────────────────┐
│ Clients                                                  │
│  Python / Jupyter   Excel / XLL   Future C ABI clients   │
└───────────────────────┬──────────────────────────────────┘
                        │
┌───────────────────────▼──────────────────────────────────┐
│ C++ orchestration layer                                  │
│ - model registry                                          │
│ - product registry                                       │
│ - measure registry                                       │
│ - API uniform across clients                              │
└───────────────────────┬──────────────────────────────────┘
                        │ FFI
┌───────────────────────▼──────────────────────────────────┐
│ Rust core                                                │
│ - Burn tensor backend                                     │
│ - Monte Carlo / analytic kernels                          │
│ - differentiation-ready numerics                         │
└──────────────────────────────────────────────────────────┘
```

## What it already does

This repo is not a finished production platform yet; it is a deliberately staged architecture experiment and prototype.

The current implementation focuses on a core use case:

- two short-rate models: Hull-White 1-factor and Hull-White 2-factor (G2++)
- interest rate swap (IRS) product
- exposure profile, PV/DV01, and unilateral CVA metrics
- a calibrator per model (`HullWhite1F`/`HullWhite2F`), fitting model parameters to a
  `MarketSnapshot` (real or fabricated) by damped Gauss-Newton with an AAD-computed Jacobian
- Python and C++ access through the same registry abstraction
- Excel XLL support in the same design philosophy

In other words, it already demonstrates the “single source of truth” principle:

- the model is defined once
- the product is defined once
- the measure is defined once
- the same registry can be consumed by multiple clients

## The core idea in one sentence

Build an XVA engine where product logic is registered once in a C++ domain layer, while Rust handles the heavy numerical work and Python/Excel only expose the same API semantics to users.

## Why the design matters

In a real XVA workflow, the issue is not only pricing an IRS at a point in time.
The real challenge is to compose:

- models
- products
- exposure profiles
- CVA/DVA/FVA/MVA/KVA metrics
- sensitivities
- client-facing interfaces without duplicating business logic

This project is structured around that problem.

## Example: exposure profile + CVA

The repository already includes a model/product/measure flow such as this:

```python
import engine

eng = engine.Engine()

model = eng.create_model("HullWhite1F", {
    "a": 0.1,
    "b": 0.03,
    "sigma": 0.01,
    "r0": 0.02,
})

product = eng.create_product("IRSwap", {
    "notional": 1_000_000.0,
    "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
    "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
})

market = engine.MarketSnapshot(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02], hazard_rate=0.02, recovery_rate=0.4)
pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 5000.0, "n_steps": 208.0, "seed": 7.0})
execution = engine.ExecutionContext({"backend": "auto", "precision": "FP64"})

result = eng.calc(product, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"], model, market, pricing, execution)

print(result["UnilateralCVA"].scalar)
# ~ 503.64
print(result["ExpectedExposure"].primary)   # expected exposure (EE), per auto-derived reset date
print(result["PFE95"].primary)              # PFE 95%
```

That is not just a toy example: it captures a realistic XVA workflow in a compact, testable shape — a batch of related measures (PV, DV01, expected exposure, PFE, CVA) computed together against the same trade/model/market/pricing/execution context, sharing the underlying Monte Carlo simulation where possible.

This is exactly the kind of data used to estimate CVA, exposure management limits, and counterparty risk over time.

## Example: calibration

Instead of picking model parameters by hand, a calibrator fits them to a `MarketSnapshot` —
here a fabricated one (`synthetic_from_hull_white*`), useful for testing/demoing calibration
without depending on real market data. `Engine.create_calibrator`/`Calibrator.calibrate` are
fully generic by name, exactly like `create_model`/`create_product`: adding `HullWhite2F`'s
calibrator required zero changes to this API surface, only a new registration in C++
`bootstrap.cpp`.

```python
import engine

eng = engine.Engine()

# HullWhite1F: calibrates (a, b); sigma/r0 stay fixed inputs.
market_1f = engine.MarketSnapshot.synthetic_from_hull_white(a=0.15, b=0.025, sigma=0.008, r0=0.02,
                                                             pillars=[0.5, 1, 2, 3, 5, 7, 10, 15, 20, 30])
calibrator_1f = eng.create_calibrator("HullWhite1F")
result_1f = calibrator_1f.calibrate(market_1f, {"a": 0.3, "b": 0.01, "sigma": 0.008, "r0": 0.02})
model_1f = eng.create_model("HullWhite1F", result_1f.optimal_params)  # Market -> calibrate -> Model

# HullWhite2F/G2++: calibrates (a, b), the two mean-reversion speeds; sigma/eta/rho/r0 fixed.
market_2f = engine.MarketSnapshot.synthetic_from_hull_white_2f(a=0.15, b=0.25, sigma=0.008, eta=0.01,
                                                                rho=-0.6, r0=0.02,
                                                                pillars=[0.5, 1, 2, 3, 5, 7, 10, 15, 20, 30])
calibrator_2f = eng.create_calibrator("HullWhite2F")
result_2f = calibrator_2f.calibrate(market_2f, {"a": 0.4, "b": 0.05, "sigma": 0.008, "eta": 0.01, "rho": -0.6, "r0": 0.02})
model_2f = eng.create_model("HullWhite2F", result_2f.optimal_params)

print(result_1f.rmse, result_1f.converged)
print(result_2f.rmse, result_2f.converged)
```

The two calibrators are not the same code with renamed fields: `HullWhite1F`'s `b` is a
long-run level (any sign), while `HullWhite2F`'s `b` is the second factor's mean-reversion
speed (must be positive, like `a`) — each calibrator reparametrizes only what it needs to keep
the optimizer unconstrained. Same story in the C ABI: `engine_abi_create_calibrator("HullWhite2F")`
+ `engine_abi_calibrate(...)` reuse the exact same generic handle-based calls as the model above
it, and in Excel: `ENGINE.CREATE_CALIBRATOR("HullWhite2F")` + `ENGINE.CALIBRATE(...)`.

## Example: Excel

The project consciously keeps the same semantics in Excel as in Python.

Conceptually:

```excel
Trade    = ENGINE.CREATE_PRODUCT("IRSwap", SwapParams)
Model    = ENGINE.CREATE_MODEL("HullWhite1F", HullWhiteParams)
Market   = ENGINE.CREATE_MARKET(MarketParams)
Pricing  = ENGINE.CREATE_CONTEXT(PricingParams)
Compute  = ENGINE.CREATE_EXECUTION(ExecutionParams)
         = ENGINE.CALC(Trade, {"PV";"DV01";"ExpectedExposure";"PFE95";"UnilateralCVA"}, Model, Market, Pricing, Compute)
```

The idea is that a trader or quant should not need a different mental model when switching from a notebook to a spreadsheet. The same model/product/measure registry should power both frontends.

## Technology stack

- Rust: numerical core and tensor-based computation
- Burn: backend abstraction for vectorized computation and autodiff
- C++: domain orchestration and registry architecture
- nanobind: Python bindings
- Excel XLL: spreadsheet integration
- CMake + Corrosion: build orchestration

## Current status

This repo is in an active prototype phase. The direction is clear and the architecture is intentionally disciplined:

- core numerical layer in Rust
- registry-first domain design in C++
- multiple clients consuming the same public API
- focus on real XVA concepts: exposure, CVA, and model/product extensibility

This is not a polished commercial product yet, but it is a strong engineering foundation for one.

## Roadmap

The project plan in `PLAN.md` outlines a progressive roadmap:

- phase 0: build skeleton and end-to-end smoke tests
- phase 1: Rust numerical core with Hull-White + IRS + CVA
- phase 2: C++ registry and measures
- phase 3: Python client
- phase 4: Excel XLL client
- phase 5: GPU backend (`burn-wgpu`) benchmarked on the IRS+Hull-White case; kept opt-in
  behind the `gpu` feature (see `PLAN.md` §7.11) rather than default, since it only pays off
  above ~100k Monte Carlo paths (`PLAN.md` §7.12, superseded by `ExecutionContext` in §7.15 —
  see below)
- phase 6: universal API as a flat, versioned C ABI (`cpp/engine/include/engine/abi.h`) —
  the same registry surface Python/Excel already consume, for languages with C FFI (Julia,
  .NET, Go, ...) without going through `cxx`/nanobind. Verified with GoogleTest and a pure-C
  smoke program, plus standalone C++/Rust/Python examples under `examples/abi/`; not yet
  published as its own release artifact (`PLAN.md` §7.13)
- phase 7: `MarketSnapshot` (a market curve, real or fabricated) and a new `ICalibrator`
  registry — calibrates `HullWhite1F`'s `a`/`b` to a curve by damped Gauss-Newton using the
  autodiff already built for sensitivities (§5.3), across all five layers: Rust, the C++
  registry, the C ABI, Python, and Excel (`PLAN.md` §7.14)
- phase 7.15: public API redesign — explicit `Market`/`PricingContext`/`ExecutionContext`
  objects and a single `ENGINE.CALC` batching PV/DV01/ExpectedExposure/PFE95/UnilateralCVA in
  one call, replacing the old `CREATE_MEASURE`+`EVALUATE` pair and the global backend state of
  phase 5, across all five layers (`PLAN.md` §7.15)
- phase 7.16: second model, Hull-White 2-factor (G2++) — a `ShortRateModel` trait unifies
  both models in Rust so `IrSwap`/`ENGINE.CALC` never special-case which one they're pricing
  (`PLAN.md` §7.16)
- phase 7.17: three levels of a future batch-calculation API (scalar / heterogeneous vector /
  homogeneous batch) — only the Rust-level homogeneous batch is implemented so far, ahead of a
  second real product (`PLAN.md` §7.17)
- phase 7.18: second calibrator, for `HullWhite2F` — a different parameter subset than
  `HullWhite1F`'s (both mean-reversion speeds vs. speed + long-run level), plus generalizing
  the C ABI's calibration surface from a `HullWhite1F`-specific function to a generic
  handle-based `engine_abi_create_calibrator`/`engine_abi_calibrate` (ABI version bumped to 3),
  across all five layers (`PLAN.md` §7.18)
- future phases: broader XVA metrics, more products

## Why this might get attention

Because it solves a real engineering pain point:

- quant libraries often become language-specific or product-specific
- XVA workflows are complex and iterative
- finance teams need the same model logic in notebooks, Excel, and production tooling

Engine Quant is trying to build that “single engine, multiple clients” experience from the start.

## Build and experimentation

This repository is organized for local experimentation and iterative architecture work.

Typical flow:

```bash
git clone https://github.com/makiolo/engine_quant.git
cd engine_quant
cmake -S . -B build -G Ninja
cmake --build build
```

The repo already contains Python tests and a registry-driven usage pattern; the project is designed to evolve from this prototype into a more complete XVA platform.

## Contributing

Contributions are welcome if they help move the project forward in any of these areas:

- more models and products
- stronger numerical validation
- better exposure analytics
- improved Python ergonomics
- Excel integration polish
- documentation and examples

If you like the idea of a single XVA engine with one registry and multiple interfaces, this project is worth following.

## License

This project is currently in active development. Please check the repository state and licensing terms before using it in production environments.
