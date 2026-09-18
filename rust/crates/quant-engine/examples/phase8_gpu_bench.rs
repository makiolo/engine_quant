//! Fase 8 GPU break-even harness.
//!
//! This prints measured Burn/WGPU probe timings when a usable adapter and f64 support exist.
//! Without them it reports an explicit pending gate; it never fabricates an Auto routing table.
//!
//! `cargo run -p quant-engine --release --features gpu --example phase8_gpu_bench`

use quant_domain::DevicePreference;
use quant_engine::gpu::{
    probe_wgpu, GpuMetricSource, GpuRouteTarget, GpuRouter, GpuRoutingPolicy, GpuTimings,
    GpuWorkload,
};

fn main() {
    let probe = probe_wgpu(None);
    println!("compiled_gpu={}", probe.compiled);
    println!("adapter_available={}", probe.available);
    println!("supports_f64={}", probe.supports_f64);
    println!("metric_source={:?}", probe.source);
    if !probe.available {
        println!(
            "GPU_UNAVAILABLE: {}",
            probe.reason.unwrap_or_else(|| "unknown".into())
        );
        println!("AUTO=CPU; gate=PENDING (no measured GPU break-even table)");
        return;
    }
    println!("cold_init_ns={}", probe.timings.cold_init_ns);
    println!("h2d_ns={}", probe.timings.h2d_ns);
    println!("kernel_ns={}", probe.timings.kernel_ns);
    println!("d2h_ns={}", probe.timings.d2h_ns);
    let warm = probe_wgpu(None);
    println!("warm_probe_init_ns={}", warm.timings.cold_init_ns);
    if probe.source != GpuMetricSource::Measured || !probe.supports_f64 {
        println!("AUTO=CPU; gate=PENDING (f64 or measured timings unavailable)");
        return;
    }

    // A small observed-point table: the probe is the only measured point here. Larger batches
    // are intentionally left as harness inputs rather than invented values.
    for batch in [1usize, 1_024, 16_384] {
        let router = GpuRouter::new(
            quant_engine::gpu::GpuInventory::new([quant_engine::gpu::GpuDeviceInfo {
                id: 0,
                label: probe.label.clone(),
                available: probe.available,
                supports_f64: probe.supports_f64,
                memory_budget_bytes: 0,
            }]),
            GpuRoutingPolicy {
                min_batch_items: 1_024,
                ..Default::default()
            },
        );
        // No CPU-vs-GPU estimate is inferred from a single scalar probe. Keep Auto on CPU until
        // callers supply real batch timings through this harness.
        let workload = GpuWorkload {
            batch_items: batch,
            gpu_bytes: (batch as u64) * 8,
            precision: quant_engine::gpu::GpuPrecision::F64,
            resident: false,
            cpu_ns: 0,
            gpu: GpuTimings {
                cold_init_ns: probe.timings.cold_init_ns,
                h2d_ns: probe.timings.h2d_ns,
                kernel_ns: probe.timings.kernel_ns,
                d2h_ns: probe.timings.d2h_ns,
            },
        };
        let decision = router
            .route(DevicePreference::Auto, workload)
            .expect("Auto never errors");
        assert_eq!(decision.target, GpuRouteTarget::Cpu);
        println!(
            "batch={batch}; auto={:?}; reason={:?}",
            decision.target, decision.reason
        );
    }
    println!(
        "AUTO=CPU; gate=PENDING (collect representative CPU/GPU batches before enabling Auto)"
    );
}
