//! Reproducible Phase-2 harness.
//!
//! This is intentionally a plain executable instead of a Criterion benchmark: it has no
//! additional dependency and emits machine-readable measurements suitable for CI/artifact
//! comparison. It reports elapsed times only; it does not assert an SLO.

use quant_domain::{MarketSpec, ModelSpec, PricingContext, ProductSpec};
use quant_engine::{
    Capabilities, ConstantKernel, Engine, EngineConfig, KernelDescriptor, KernelId, KernelRegistry,
    PlanRequest, ProviderId,
};
use serde_json::json;
use std::{hint::black_box, sync::Arc, time::Instant};

const ITERATIONS: usize = 200;

fn elapsed_ns<F: FnMut()>(mut f: F) -> u128 {
    let start = Instant::now();
    for _ in 0..ITERATIONS {
        f();
    }
    start.elapsed().as_nanos()
}

fn request() -> PlanRequest {
    PlanRequest::inline(
        "m",
        MarketSpec::new(
            json!({"r0": 0.02, "curve": (0..2048).map(|i| i as f64 * 0.0001).collect::<Vec<_>>() }),
        )
        .expect("market object"),
        "hw",
        ModelSpec::from_value(json!({"a": 0.1, "sigma": 0.01})).expect("model object"),
        "irs",
        ProductSpec::from_value(json!({"payment_times": [1.0, 2.0], "accruals": [1.0, 1.0]}))
            .expect("product object"),
        "PV",
        PricingContext::default(),
    )
}

fn registry() -> KernelRegistry {
    let descriptor = KernelDescriptor {
        id: KernelId::from("phase2-bench-kernel"),
        provider: ProviderId::from("rust-bench"),
        model_kinds: vec!["hull_white_1f".into()],
        product_kinds: vec!["ir_swap".into()],
        measures: vec!["PV".into()],
        capabilities: Capabilities::empty(),
        input_layout_version: 1,
        priority: 0,
    };
    let mut registry = KernelRegistry::new();
    registry.register(Arc::new(ConstantKernel::new(descriptor, 1.0)));
    registry
}

fn main() {
    let request = request();

    // Cold compile is measured before the first cached lookup. The second measurement is
    // deliberately a cache hit on the same semantic request.
    let cold_registry = registry();
    let cold_start = Instant::now();
    let cold_plan = cold_registry.compile_plan(&request).expect("cold plan");
    let cold_ns = cold_start.elapsed().as_nanos();
    let hit_ns = elapsed_ns(|| {
        black_box(cold_registry.compile_plan(&request).expect("cached plan"));
    });
    let stats = cold_registry.cache_stats();

    // Handles clone an Arc; copying the equivalent market spec clones the complete map and
    // curve vector. Both loops use black_box to prevent dead-code elimination.
    let handle = request.market.clone();
    let handle_ns = elapsed_ns(|| {
        black_box(handle.clone());
    });
    let copied_market = request.market.snapshot().spec.clone();
    let copy_ns = elapsed_ns(|| {
        black_box(copied_market.clone());
    });

    // Compare the new one-item batch path with the unchanged phase-1 PricingInput API.
    let engine = Engine::with_providers(
        EngineConfig::default(),
        vec![Arc::new(ConstantKernel::new(
            KernelDescriptor {
                id: KernelId::from("phase2-price-kernel"),
                provider: ProviderId::from("rust-bench"),
                model_kinds: vec!["hull_white_1f".into()],
                product_kinds: vec!["ir_swap".into()],
                measures: vec!["PV".into()],
                capabilities: Capabilities::empty(),
                input_layout_version: 1,
                priority: 0,
            },
            1.0,
        ))],
    )
    .expect("engine");
    let typed_ns = elapsed_ns(|| {
        black_box(engine.price_typed(request.clone()).expect("typed price"));
    });

    let context = quant_domain::QuantContext::new("phase2-bench").apply(&[
        quant_domain::ContextCommand::AddMarket { id: "m".into(), spec: json!({"r0": 0.02}) },
        quant_domain::ContextCommand::AddModel { id: "hw".into(), spec: json!({"a": 0.1, "b": 0.03, "sigma": 0.01}) },
        quant_domain::ContextCommand::AddProduct { id: "irs".into(), spec: json!({"notional": 1_000_000.0, "fixed_rate": 0.02, "payment_times": [1.0, 2.0], "accruals": [1.0, 1.0]}) },
    ]).expect("context").0;
    let old_input = quant_domain::PricingInput {
        context,
        operation_id: "phase2-bench".into(),
        market: Some(quant_domain::ResourceRef {
            id: "m".into(),
            hash: String::new(),
            kind: "market".into(),
            version: 1,
        }),
        model: Some(quant_domain::ResourceRef {
            id: "hw".into(),
            hash: String::new(),
            kind: "model".into(),
            version: 1,
        }),
        products: vec![quant_domain::ResourceRef {
            id: "irs".into(),
            hash: String::new(),
            kind: "product".into(),
            version: 1,
        }],
        measures: vec![quant_domain::MeasureSpec {
            name: "PV".into(),
            params: Default::default(),
        }],
        pricing: Default::default(),
        execution: Default::default(),
        output: Default::default(),
    };
    let old_ns = elapsed_ns(|| {
        black_box(engine.price(old_input.clone()).expect("phase-1 price"));
    });

    let output = json!({
        "schema": "quant.phase2-benchmark/v1",
        "iterations": ITERATIONS,
        "measurements": {
            "compile_plan_cold_ns": cold_ns,
            "compile_plan_cache_hit_total_ns": hit_ns,
            "compile_plan_cache_hit_mean_ns": hit_ns / ITERATIONS as u128,
            "handle_arc_clone_total_ns": handle_ns,
            "market_spec_copy_total_ns": copy_ns,
            "price_typed_batch_one_total_ns": typed_ns,
            "price_phase1_total_ns": old_ns,
        },
        "cache": {"hits": stats.hits, "misses": stats.misses},
        "plan": {"fingerprint": cold_plan.fingerprint, "batches": cold_plan.batches.len()},
        "notes": ["Wall-clock measurements on the invoking machine; no SLO or speedup claim is made.", "Run repeatedly on controlled hardware for comparisons."],
    });
    println!(
        "{}",
        serde_json::to_string_pretty(&output).expect("benchmark JSON")
    );
}
