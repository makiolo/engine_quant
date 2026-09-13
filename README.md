<div align="center">

# Engine Quant

**One XVA engine. Rust numerics. Consistent APIs for Python, C++, C, and Excel.**

[![CI](https://github.com/makiolo/engine_quant/actions/workflows/ci.yml/badge.svg)](https://github.com/makiolo/engine_quant/actions/workflows/ci.yml)
[![Rust 1.97.1](https://img.shields.io/badge/Rust-1.97.1-000000?logo=rust)](rust/rust-toolchain.toml)
[![Python 3.9+](https://img.shields.io/badge/Python-3.9%2B-3776AB?logo=python&logoColor=white)](pyproject.toml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)](CMakeLists.txt)

Research-oriented pricing, exposure, and counterparty-credit analytics built around a
single registry-driven domain model.

</div>

> [!IMPORTANT]
> Engine Quant is an active research prototype, not a production-ready risk system. Its
> current scope is deliberately narrow: short-rate models, vanilla interest-rate swaps,
> exposure profiles, and unilateral CVA.

## Why Engine Quant?

Quant engines often grow into separate implementations for notebooks, spreadsheets, and
production services. Engine Quant keeps the business semantics in one place:

- models, products, measures, and calibrators are registered centrally;
- Python, Excel, C++, and the public C ABI consume the same C++ orchestration layer;
- compute-intensive pricing and Monte Carlo kernels live in Rust;
- related measures can be calculated together and share the same simulation.

The result is one vocabulary and one calculation path across every client.

## Current capabilities

| Area | Implemented |
| --- | --- |
| Models | `HullWhite1F`; `HullWhite2F` / G2++ |
| Products | Vanilla interest-rate swap (`IRSwap`) and generic composable payoff (`Payoff`) |
| Measures | `PV`, `DV01`, `ExpectedExposure`, `PFE95`, `UnilateralCVA` |
| Calibration | Registry-based calibrators for both short-rate models, using damped Gauss-Newton and AAD Jacobians |
| Compute | Burn tensor backend; CPU by default; opt-in WGPU backend |
| Clients | Python extension, Excel XLL, native C++ API, and versioned C ABI |
| Distribution | Windows wheels, Excel add-in package, and all-in-one Inno Setup installer produced by the release workflow |

`Engine.price(...)` accepts a batch of measure names. `ExpectedExposure` and `PFE95`, for
example, reuse one exposure simulation rather than running Monte Carlo twice. `price_batch`/
`price_many`/`price_grid` extend that batching across trades, and across models and markets.

## Architecture

```text
┌────────────────────┐  ┌────────────────────┐  ┌────────────────────┐
│ Python / Jupyter   │  │ Excel XLL          │  │ C ABI consumers    │
│ nanobind module    │  │ worksheet UDFs     │  │ C/Rust/Python/...  │
└─────────┬──────────┘  └─────────┬──────────┘  └─────────┬──────────┘
          └───────────────────────┼───────────────────────┘
                                  ▼
              ┌───────────────────────────────────────┐
              │ C++17 domain and orchestration layer │
              │ registries · contexts · batched price │
              └───────────────────┬───────────────────┘
                                  │ cxx bridge
                                  ▼
              ┌───────────────────────────────────────┐
              │ Rust numerical core                   │
              │ pricing · Monte Carlo · AAD · Burn   │
              └───────────────────────────────────────┘
```

The public C ABI is intentionally separate from the internal Rust/C++ `cxx` bridge. It
exposes flat, versioned types and opaque handles so other languages can consume the engine
without depending on C++ classes or nanobind.

## Quick start with Python

Building the Python extension from source requires Git, CMake 3.24+, Ninja, a C++17
toolchain, Rust/rustup, and Python 3.9+.

```bash
git clone https://github.com/makiolo/engine_quant.git
cd engine_quant
python -m pip install .
```

`engine-quant` installs two packages: the compiled `engine` extension, and `engine_typed` —
a pure-Python, `pydantic`-backed facade for building `Trade`/`Model`/`Market`/
`PricingContext`/`ExecutionContext` with real validation instead of raw dicts. It is the
recommended way to use the engine from Python; see [Dynamic dict facade](#dynamic-dict-facade)
below for the lower-level alternative that Excel and the C ABI use.

```python
import engine
import engine_typed as q

eng = engine.Engine()

model = q.HullWhite1F(a=0.10, b=0.03, sigma=0.01, r0=0.02)

# Omitting fixed_rate is a validation error, not a par swap — use IRSwap.par(...) for that.
trade = q.IRSwap(
    notional=1_000_000.0,
    fixed_rate=0.02,
    payment_times=[1.0, 2.0, 3.0, 4.0, 5.0],
    accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
)

market = q.Market(
    pillars=[1.0, 2.0],
    zero_rates=[0.02, 0.02],
    hazard_rate=0.02,
    recovery_rate=0.40,
)
pricing = q.PricingContext(n_paths=5_000, n_steps=208, seed=7)
execution = q.ExecutionContext(backend="auto")

eng_model = eng.create_model(model.model_type, model.to_params())
eng_trade = eng.create_product(trade.product_type, trade.to_params())
eng_market = engine.MarketSnapshot(**market.to_params())
eng_pricing = engine.PricingContext(pricing.to_params())
eng_execution = engine.ExecutionContext(execution.to_params())

results = eng.price(
    eng_trade,
    ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"],
    eng_model,
    eng_market,
    eng_pricing,
    eng_execution,
)

print(results["PV"].scalar)
print(results["ExpectedExposure"].times)
print(results["ExpectedExposure"].primary)
print(results["UnilateralCVA"].scalar)
```

Discover the registered surface at runtime with `list_models()`, `list_products()`,
`list_measures()`, and `list_calibrators()`.

### Dynamic dict facade

`engine_typed` translates to the same `Params`/dict that `engine.Engine` already accepts
directly — the facade Excel and the C ABI use, and still valid from Python for quick scripts
or raw JSON:

```python
import engine

eng = engine.Engine()
model = eng.create_model("HullWhite1F", {"a": 0.10, "b": 0.03, "sigma": 0.01, "r0": 0.02})
# Omitting fixed_rate here means "par swap" — the one ambiguity engine_typed.IRSwap removes.
trade = eng.create_product(
    "IRSwap",
    {
        "notional": 1_000_000.0,
        "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
        "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
    },
)
market = engine.MarketSnapshot(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02], hazard_rate=0.02, recovery_rate=0.40)
pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 5_000, "n_steps": 208, "seed": 7})
execution = engine.ExecutionContext({"backend": "auto", "precision": "FP64"})

results = eng.price(trade, ["PV", "DV01"], model, market, pricing, execution)
print(results["PV"].scalar)
```

### Custom products from Python

Custom products are represented by `engine_typed.PayoffProduct`: compose a contract from
expressions, predicates, and timed cashflows, then register it with the generic `"Payoff"`
product type. The typed builders validate the tree before it crosses into the native engine.

For example, this creates a one-year European call on an observable named
`EQ.SPOT.AAPL`:

```python
import engine
import engine_typed as q

eng = engine.Engine()

call = q.when(
    1.0,
    q.cashflow(
        "USD",
        1_000 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100.0, 0.0),
    ),
)
trade = q.PayoffProduct(id="AAPL_CALL_100", contract=call)
product = eng.create_product(trade.product_type, trade.to_params())

print(product.type_name())  # Payoff
```

The same contract nodes can be combined to create path-dependent products. For example, this
adds a discrete up-and-in barrier and pays the call only if the barrier is hit:

```python
barrier_call = q.trigger(
    id="UP_AND_IN",
    monitoring_times=[0.25, 0.5, 0.75, 1.0],
    condition=q.greater_equal(q.current("EQ.SPOT.AAPL"), 120.0),
    monitoring="discrete",
    settlement="at_scheduled_payment",
    priority=0,
    latch=True,
    on_hit=call,
    on_miss=q.zero(),
)
barrier_trade = q.PayoffProduct(id="AAPL_UP_AND_IN", contract=barrier_call)
barrier_product = eng.create_product(
    barrier_trade.product_type,
    barrier_trade.to_params(),
)
```

For integrations that already produce JSON, the lower-level facade accepts the same
`engine.payoff/v1` envelope:

```python
import json

spec = {
    "schema": "engine.payoff/v1",
    "id": "FIXED_LEG_2Y",
    "contract": {
        "type": "both",
        "children": [
            {"type": "when", "time": 1.0, "child": {
                "type": "cashflow", "currency": "USD",
                "amount": {"type": "constant", "value": 30_000.0},
            }},
            {"type": "when", "time": 2.0, "child": {
                "type": "cashflow", "currency": "USD",
                "amount": {"type": "constant", "value": 30_000.0},
            }},
        ],
    },
}
fixed_leg = eng.create_product("Payoff", {"spec": json.dumps(spec)})
```

See the [payoff schema examples](docs/schema/engine.payoff/examples/) for more contract
shapes. Custom payoff creation and validation are available from Python; the generic payoff
valuation API is currently exposed through the native C++ payoff namespace, while
`Engine.price(...)` continues to cover the specialized `IRSwap` flow.

## Calibration

Calibrators use the same registry pattern as models and products. Their output can be
passed directly to `create_model`:

```python
pillars = [0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0]
market = engine.MarketSnapshot.synthetic_from_hull_white(
    a=0.15, b=0.025, sigma=0.008, r0=0.02, pillars=pillars,
)

initial_guess = q.HullWhite1F(a=0.30, b=0.01, sigma=0.008, r0=0.02)
calibrator = eng.create_calibrator("HullWhite1F")
fit = calibrator.calibrate(market, initial_guess.to_params())
calibrated_model = eng.create_model("HullWhite1F", q.HullWhite1F(**fit.optimal_params).to_params())

print(fit.rmse, fit.iterations, fit.converged)
```

The one-factor calibrator fits `a` and `b`; `sigma` and `r0` remain fixed. The two-factor
calibrator fits the two mean-reversion speeds (`a` and `b`), while `sigma`, `eta`, `rho`,
and `r0` remain fixed. The synthetic-market helpers exist for deterministic examples and
tests; they are not a substitute for a real calibration instrument set.

## Batch and grid calculation

Three layers, each built on the one before, let a single call price a portfolio instead of
looping over `price(...)` per trade:

- **`price_batch`** — a *homogeneous* batch: every trade must share the same product type and,
  for `IRSwap`, the same schedule (`start`/`payment_times`/`accruals`) and an explicit
  `fixed_rate` (no par-rate trades in a batch). The short-rate path is simulated once for the
  whole batch, not once per trade.
- **`price_many`** — a *heterogeneous* list: trades may mix schedules or (in the future)
  product types. They are grouped internally and each group is priced with `price_batch`;
  heterogeneity never raises an error.
- **`price_grid`** — the full **Trades × Models × Markets** combination: `price_many` runs once
  per (model, market) pair. `PricingContext`/`ExecutionContext` are shared, not part of the
  grid.

All three return one row per trade (and, for `price_grid`, per model/market too) with its
index attached explicitly — never a nested list:

```python
schedule = {"payment_times": [1.0, 2.0, 3.0, 4.0, 5.0], "accruals": [1.0] * 5}
trades = [
    eng.create_product("IRSwap", q.IRSwap(notional=1_000_000.0, fixed_rate=0.02, **schedule).to_params()),
    eng.create_product("IRSwap", q.IRSwap(notional=2_500_000.0, fixed_rate=0.015, **schedule).to_params()),
]

for row in eng.price_batch(trades, ["PV", "UnilateralCVA"], eng_model, eng_market, eng_pricing, eng_execution):
    print(row.trade_index, row.measures["PV"].scalar, row.measures["UnilateralCVA"].scalar)
```

`PV`/`DV01` discount from the observed `MarketSnapshot` curve, not the model (PLAN_REAPI.md
§6 Phase 4) — the swap is replicated in zero-coupon bonds via `discount_factor(t)`, so batching
these two is genuinely cheap: no model dispatch, no Monte Carlo, no autodiff, just one
`discount_factor` call per cash-flow date per trade. `DV01` is bump-and-reval, not AAD: a
configurable parallel bump (`Params["bump"]`, default 0.0001) shifts every `zero_rates` point,
and the batch reprices with the bumped curve once for the whole batch, same cost model as `PV`.

## Build and test from source

The repository pins Rust `1.97.1`. When rustup is installed, Cargo picks it up from
`rust/rust-toolchain.toml` automatically.

### Rust core

```bash
cargo test --manifest-path rust/Cargo.toml --workspace --locked
```

The GPU feature is kept opt-in. It can be compile-checked without a physical GPU:

```bash
cargo check --manifest-path rust/Cargo.toml \
  -p engine-core --features gpu --locked --examples
```

### Full CMake stack

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure -C Release
```

The default build includes C++ tests and, on Windows, the Excel XLL. Useful options are:

| CMake option | Default | Purpose |
| --- | --- | --- |
| `ENGINE_QUANT_BUILD_TESTS` | `ON` | Build C++ and Excel bridge tests |
| `ENGINE_QUANT_BUILD_EXCEL` | `ON` | Build the Excel client on Windows |
| `ENGINE_QUANT_ENABLE_GPU` | `OFF` | Compile the WGPU backend |

To run the Python checks against the module produced in `build/clients/python`:

```bash
python clients/python/tests/test_smoke.py build/clients/python
python clients/python/tests/test_registry.py build/clients/python
python clients/python/tests/test_price.py build/clients/python
python clients/python/tests/test_calibration.py build/clients/python
```

CI runs the Rust suite on Linux and the complete CMake, C++, C ABI, Python, and Excel-XLL
harness suite on Windows. The XLL harness loads the compiled add-in without requiring Excel
to be installed.

## Clients and examples

- [Python package notes](clients/python/README_PYPI.md)
- [Python calculation example](clients/python/examples/README.md)
- [Jupyter notebook notes](clients/python/notebooks/README.md)
- [Excel XLL guide](clients/excel/README.md)
- [Excel add-in installation](clients/excel/install/README.md)
- [C ABI examples in C, C++, Rust, ctypes, and cffi](examples/abi/README.md)
- [Windows all-in-one installer](installer/README.md)

Excel exposes the same object flow through handles and worksheet functions:

```excel
=ENGINE.CREATE_MODEL("HullWhite1F", ModelParams)
=ENGINE.CREATE_PRODUCT("IRSwap", TradeParams)
=ENGINE.CREATE_MARKET(MarketParams)
=ENGINE.CREATE_CONTEXT(PricingParams)
=ENGINE.CREATE_EXECUTION(ExecutionParams)
=ENGINE.PRICE(Trade, Measures, Model, Market, Pricing, Execution)
```

## Repository layout

```text
rust/crates/engine-core/   Numerical models, products, exposure, CVA, calibration, AAD
rust/crates/engine-ffi/    Internal Rust ↔ C++ bridge
cpp/engine/                C++ registries, contexts, batched calculation, public C ABI
clients/python/            nanobind extension, tests, examples, and notebook
clients/excel/             Excel XLL, bridge tests, and install scripts
examples/abi/              External-language examples for the stable C ABI
installer/                 Inno Setup packaging for Windows
.github/workflows/         CI and release pipelines
PLAN.md                    Architectural decisions and implementation history
```

## Scope and known limitations

- The specialized pricing flow currently covers `IRSwap`; generic `Payoff` products support
  composable contract creation/validation and have native C++ payoff valuation APIs. DVA, FVA,
  MVA, and KVA remain roadmap items.
- `pricing_date` is currently metadata. Calendar generation and day-count arithmetic are
  not implemented.
- `PV` and `DV01` discount from the observed `MarketSnapshot` curve and no longer depend on
  the model at all (PLAN_REAPI.md §6 Phase 4); `ExpectedExposure`/`PFE95`/`UnilateralCVA`
  still do (Monte Carlo, revaluing at future dates where no market curve is observable) — this
  asymmetry is intentional. The curve also feeds calibration; hazard and recovery data feed
  unilateral CVA.
- Calibration currently targets a discount curve. Production calibration to instruments
  such as swaptions or caps is outside the present scope.
- The Excel client and packaged release artifacts target 64-bit Windows. The Rust core and
  non-Excel CMake targets are designed to remain portable.
- GPU support is experimental and must be enabled explicitly. `backend="auto"` resolves to
  GPU only in a GPU-enabled build; otherwise it resolves to CPU.
- Per-measure configuration (`DV01(bump=...)`/`DV01(bucketed=True)`) is only reachable from
  Python (`engine_typed`, or a `(name, params)` tuple against `Engine.price`) — Excel and the
  C ABI still take plain measure names.

See [PLAN.md](PLAN.md) for the detailed architecture record, numerical-validation strategy,
completed phases, and future work.

## Contributing

Contributions are welcome, especially around additional products and XVA measures,
numerical validation, market-data integration, and client ergonomics. Please keep the
single-source-of-truth rule: clients should expose domain behavior, not reimplement it.

## License

This repository does not currently include a license file. Do not assume permission to use,
modify, or redistribute it outside the rights granted by applicable law.
