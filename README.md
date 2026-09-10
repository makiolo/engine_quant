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
| Products | Vanilla interest-rate swap (`IRSwap`), par or explicit fixed rate |
| Measures | `PV`, `DV01`, `ExpectedExposure`, `PFE95`, `UnilateralCVA` |
| Calibration | Registry-based calibrators for both short-rate models, using damped Gauss-Newton and AAD Jacobians |
| Compute | Burn tensor backend; CPU by default; opt-in WGPU backend |
| Clients | Python extension, Excel XLL, native C++ API, and versioned C ABI |
| Distribution | Windows wheels, Excel add-in package, and all-in-one Inno Setup installer produced by the release workflow |

`Engine.calc(...)` accepts a batch of measure names. `ExpectedExposure` and `PFE95`, for
example, reuse one exposure simulation rather than running Monte Carlo twice.

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
              │ registries · contexts · batched calc │
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

Then create a model, trade, market, and explicit calculation contexts:

```python
import engine

eng = engine.Engine()

model = eng.create_model(
    "HullWhite1F",
    {"a": 0.10, "b": 0.03, "sigma": 0.01, "r0": 0.02},
)

# Omitting fixed_rate creates a par swap under the selected model.
trade = eng.create_product(
    "IRSwap",
    {
        "notional": 1_000_000.0,
        "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
        "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
    },
)

market = engine.MarketSnapshot(
    pillars=[1.0, 2.0],
    zero_rates=[0.02, 0.02],
    hazard_rate=0.02,
    recovery_rate=0.40,
)
pricing = engine.PricingContext(
    {"pricing_date": 0.0, "n_paths": 5_000, "n_steps": 208, "seed": 7}
)
execution = engine.ExecutionContext({"backend": "auto", "precision": "FP64"})

results = eng.calc(
    trade,
    ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"],
    model,
    market,
    pricing,
    execution,
)

print(results["PV"].scalar)
print(results["ExpectedExposure"].times)
print(results["ExpectedExposure"].primary)
print(results["UnilateralCVA"].scalar)
```

Discover the registered surface at runtime with `list_models()`, `list_products()`,
`list_measures()`, and `list_calibrators()`.

## Calibration

Calibrators use the same registry pattern as models and products. Their output can be
passed directly to `create_model`:

```python
pillars = [0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0]
market = engine.MarketSnapshot.synthetic_from_hull_white(
    a=0.15,
    b=0.025,
    sigma=0.008,
    r0=0.02,
    pillars=pillars,
)

calibrator = eng.create_calibrator("HullWhite1F")
fit = calibrator.calibrate(
    market,
    {"a": 0.30, "b": 0.01, "sigma": 0.008, "r0": 0.02},
)
calibrated_model = eng.create_model("HullWhite1F", fit.optimal_params)

print(fit.rmse, fit.iterations, fit.converged)
```

The one-factor calibrator fits `a` and `b`; `sigma` and `r0` remain fixed. The two-factor
calibrator fits the two mean-reversion speeds (`a` and `b`), while `sigma`, `eta`, `rho`,
and `r0` remain fixed. The synthetic-market helpers exist for deterministic examples and
tests; they are not a substitute for a real calibration instrument set.

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
python clients/python/tests/test_calc.py build/clients/python
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
=ENGINE.CALC(Trade, Measures, Model, Market, Pricing, Execution)
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

- Only one product (`IRSwap`) and unilateral CVA are implemented; DVA, FVA, MVA, and KVA
  remain roadmap items.
- `pricing_date` is currently metadata. Calendar generation and day-count arithmetic are
  not implemented.
- `PV` and `DV01` are model-based and do not yet discount from the `MarketSnapshot` curve.
  The curve is used by calibration; hazard and recovery data feed unilateral CVA.
- Calibration currently targets a discount curve. Production calibration to instruments
  such as swaptions or caps is outside the present scope.
- The Excel client and packaged release artifacts target 64-bit Windows. The Rust core and
  non-Excel CMake targets are designed to remain portable.
- GPU support is experimental and must be enabled explicitly. `backend="auto"` resolves to
  GPU only in a GPU-enabled build; otherwise it resolves to CPU.

See [PLAN.md](PLAN.md) for the detailed architecture record, numerical-validation strategy,
completed phases, and future work.

## Contributing

Contributions are welcome, especially around additional products and XVA measures,
numerical validation, market-data integration, and client ergonomics. Please keep the
single-source-of-truth rule: clients should expose domain behavior, not reimplement it.

## License

This repository does not currently include a license file. Do not assume permission to use,
modify, or redistribute it outside the rights granted by applicable law.
