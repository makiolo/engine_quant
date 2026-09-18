# `quant-cpp` — Fase 3 bridge

`quant-cpp` is a Rust-hosted `cxx` provider. The only C++ surface visible to Rust is
`cpp/quant-cpp/include/quant_cpp_bridge.hpp`; the validated legacy IRS/Hull-White leaf is behind
that shim in `legacy_irs_leaf.hpp`. The crate compiles the leaf directly with `cxx-build`, so its
Cargo build never invokes top-level CMake/Corrosion and cannot form a Cargo → CMake → Cargo cycle.

## Contract

Bridge version 1 uses `BatchShape { trades, scenarios, factors, times }`. Inputs are borrowed
contiguous `f64` slices for the call only:

- `market = [r0]`;
- `model = [a, b, sigma]`;
- each product is `[notional, fixed_rate, start, use_par_rate]`;
- `payment_times` and `accruals` are parallel schedule slices;
- Rust owns and preallocates `output`, one value per trade.

The C++ kernel retains no input pointer. `CppKernelAdapter` owns the immutable C++ kernel in a
`UniquePtr` and is explicitly `Send + Sync` because the leaf has no mutable global or per-call
state. A future stateful legacy kernel must use worker confinement instead of copying this unsafe
contract.

All fallible calls return `CppStatus`; the shim catches `bad_alloc`, standard exceptions and
unknown exceptions. Errors are categorized as invalid argument, unsupported, numerical failure,
resource exhausted or internal. No exception or panic crosses the pricing call.

## Verification and benchmark harness

```text
cargo test -p quant-cpp
cargo run -p quant-cpp --release --example bridge_bench
```

The tests cover empty and large buffers, schedule/shape errors, non-finite/numerical failures,
destruction/lifetime by ownership, concurrent calls and Rust-vs-C++ IRS PV parity (including the
par-rate convention). The benchmark prints no-op, scalar and batch bridge timings, borrowed
inputs versus explicit vector copies, and Rust-preallocated output versus an explicit output-copy
proxy. It reports observations only; no SLO or break-even claim is invented from CI hardware.

## Sanitizers

On Linux/Clang, run the crate with the C++ flags below (and a clean target directory) to exercise
the same leaf under ASan/UBSan:

```sh
RUSTFLAGS="-Zsanitizer=address" \
  CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  cargo +nightly test -p quant-cpp -Zbuild-std --target x86_64-unknown-linux-gnu
```

The repository's Windows CI uses MSVC. MSVC does not provide the same combined ASan/UBSan gate
or `-Zsanitizer` flow; use the Visual Studio AddressSanitizer configuration locally and keep the
Linux gate as the portable sanitizer check when a compatible runner is available.
