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

- Hull-White 1-factor model
- interest rate swap (IRS) product
- exposure profile and unilateral CVA metrics
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

measure = eng.create_measure("UnilateralCVA")
result = measure.evaluate(
    model,
    product,
    {
        "monitoring_times": [0.0, 1.0, 2.0, 3.0],
        "n_paths": 5000.0,
        "seed": 13.0,
        "hazard_rate": 0.02,
        "recovery_rate": 0.4,
    },
)

print(result.scalar)
# ~ 426.76
```

That is not just a toy example: it captures a realistic XVA workflow in a compact, testable shape.

A more “analytics-first” view is the exposure profile itself:

```python
measure = eng.create_measure("ExposureProfile")
profile = measure.evaluate(
    model,
    product,
    {
        "monitoring_times": [0.0, 1.0, 2.0],
        "n_paths": 5000.0,
        "seed": 7.0,
    },
)

print(profile.primary)   # expected exposure (EE)
print(profile.secondary)  # PFE 95%
```

This is exactly the kind of data used to estimate CVA, exposure management limits, and counterparty risk over time.

## Example: Excel

The project consciously keeps the same semantics in Excel as in Python.

Conceptually:

```excel
=ENGINE.CREATE_MODEL("HullWhite1F", RangoHullWhite)
=ENGINE.CREATE_PRODUCT("IRSwap", RangoIRS)
=ENGINE.CREATE_MEASURE("UnilateralCVA")
=ENGINE.EVALUATE(handleMedida, handleModelo, handleProducto, RangoParametros)
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
  above ~100k Monte Carlo paths. Selectable from clients: `with engine.backend("gpu"):` in
  Python, `ENGINE.SET_BACKEND("gpu")` in Excel (`PLAN.md` §7.12)
- phase 6: universal API as a flat, versioned C ABI (`cpp/engine/include/engine/abi.h`) —
  the same registry/backend-selection surface Python/Excel already consume, for languages
  with C FFI (Julia, .NET, Go, ...) without going through `cxx`/nanobind. Verified with
  GoogleTest and a pure-C smoke program, plus standalone C++/Rust/Python examples under
  `examples/abi/` that all produce the same numbers; not yet published as its own release
  artifact (`PLAN.md` §7.13)
- phase 7: `MarketSnapshot` (a market curve, real or fabricated) and a new `ICalibrator`
  registry — calibrates `HullWhite1F`'s `a`/`b` to a curve by damped Gauss-Newton using the
  autodiff already built for sensitivities (§5.3), across all five layers: Rust, the C++
  registry, the C ABI, Python, and Excel (`PLAN.md` §7.14)
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
