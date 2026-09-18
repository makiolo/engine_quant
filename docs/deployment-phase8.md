# Fase 8 — GPU opcional

`quant-engine` exposes the `gpu` feature, which forwards `engine-core/gpu` and enables the
existing Burn 0.21 WGPU backend:

```text
cargo check -p quant-engine --features gpu
cargo run -p quant-engine --release --features gpu --example phase8_gpu_bench
```

The default build has no WGPU dependency and keeps `DevicePreference::Auto` on CPU. The
`quant_engine::gpu` module owns device discovery, routing, queue admission, memory budgets,
and stage metrics; no WGPU type crosses into `quant-domain` products or models.

Routing rules are explicit:

- `Cpu` always selects CPU.
- `Gpu` errors on an unavailable device, unsupported required f64, queue, or memory condition
  unless `GpuFallbackPolicy::Cpu` is explicitly selected. A fallback is returned as a decision
  with `fallback_used=true` and a reason.
- `Auto` selects GPU only when an available device preserves precision, memory admission passes,
  the batch threshold is met, and measured/proxy timings show the configured net benefit. It
  otherwise stays on CPU with a reason.

Per-device queue capacity and resident-memory budgets are enforced by `GpuScheduler`; permits
release both counters on completion/drop. `GpuMetricsSnapshot` carries a `Measured` or `Proxy`
source tag for cold-init, H2D, kernel, and D2H timings.

## Gate status

`phase8_gpu_bench` reports only observations from the current machine. It prints an explicit
`GPU_UNAVAILABLE`/`AUTO=CPU; gate=PENDING` status when no usable adapter or f64 support exists,
and it does not invent an Auto routing table. The final gate remains pending until representative
CPU-vs-GPU batches (including resident/non-resident and real f64) establish break-even data.

Fase 9 must consume the selected device/fallback decision and resource metrics; it must not add
XVA semantics to this module.
