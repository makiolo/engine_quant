//! Phase-9 XVA benchmark harness.
//!
//! Unlike the planning-only Python harness, this executable invokes the real
//! `Engine::calculate_xva` pipeline.  It compares one shared call with separate
//! measure calls, and a materialized synthetic cube with bounded chunk calls.

use quant_domain::{
    ContextHash, ExposureCubeInput, ProbabilityMeasure, QuantContext, ResourceRef, XvaMeasure,
    XvaRequest,
};
use quant_engine::{estimate_memory_bytes, Engine, EngineConfig};
use serde::Serialize;
use serde_json::json;
use std::time::Instant;

#[derive(Debug, Serialize)]
struct BenchmarkResult {
    schema: &'static str,
    times: usize,
    paths: usize,
    chunk_size: usize,
    shared_pipeline_ms: f64,
    separate_measures_ms: f64,
    materialized_ms: f64,
    streaming_ms: f64,
    materialized_bytes_estimate: u64,
    streaming_peak_bytes_estimate: u64,
    materialized_artifact_lineage: Vec<String>,
    streaming_artifact_lineage: Vec<String>,
    shared_graph: bool,
    arrow_output: &'static str,
    cva_shared: f64,
    cva_streaming: f64,
    streaming_matches_materialized: bool,
}

fn arg(args: &[String], name: &str, default: usize) -> usize {
    args.windows(2)
        .find(|pair| pair[0] == name)
        .and_then(|pair| pair[1].parse().ok())
        .unwrap_or(default)
}

fn cube(
    context_hash: Option<ContextHash>,
    offset: usize,
    times: usize,
    paths: usize,
) -> ExposureCubeInput {
    let grid = (offset..offset + times)
        .map(|index| (index + 1) as f64 / 12.0)
        .collect::<Vec<_>>();
    // Deterministic synthetic exposure with both positive and negative paths.
    let values = grid
        .iter()
        .enumerate()
        .map(|(row, time)| {
            (0..paths)
                .map(|path| {
                    let sign = if (offset + row + path).is_multiple_of(5) {
                        -1.0
                    } else {
                        1.0
                    };
                    sign * (100.0 + *time * 10.0 + (path % 17) as f64)
                })
                .collect::<Vec<_>>()
        })
        .collect::<Vec<_>>();
    ExposureCubeInput {
        measure: ProbabilityMeasure::Q,
        times: grid,
        paths: values,
        discount_factors: Vec::new(),
        source_hash: context_hash,
    }
}

fn request(
    context: &QuantContext,
    exposure: ExposureCubeInput,
    measures: Vec<XvaMeasure>,
    materialize: bool,
) -> XvaRequest {
    XvaRequest {
        context: context.clone(),
        operation_id: "phase9-bench".into(),
        portfolio_id: "bench-book".into(),
        market: ResourceRef {
            id: "bench-market".into(),
            hash: "bench-market-hash".into(),
            kind: "market".into(),
            version: 1,
        },
        model: ResourceRef {
            id: "bench-model".into(),
            hash: "bench-model-hash".into(),
            kind: "model".into(),
            version: 1,
        },
        netting_sets: Vec::new(),
        collateral_agreements: Vec::new(),
        counterparty_default: quant_domain::DefaultSpec {
            hazard_rate: 0.02,
            recovery_rate: 0.4,
            ..Default::default()
        },
        own_default: quant_domain::DefaultSpec {
            hazard_rate: 0.02,
            recovery_rate: 0.4,
            measure: ProbabilityMeasure::P,
            ..Default::default()
        },
        funding: Default::default(),
        measures,
        confidence_level: 0.95,
        q_exposure: Some(exposure),
        p_exposure: None,
        materialize_exposure: materialize,
        memory_budget_bytes: None,
        pricing: Default::default(),
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = std::env::args().collect::<Vec<_>>();
    let times = arg(&args, "--times", 24).max(1);
    let paths = arg(&args, "--paths", 256).max(1);
    let chunk_size = arg(&args, "--chunk", 6).max(1).min(times);
    let engine = Engine::new(EngineConfig::default())?;
    let context = QuantContext::new("phase9-benchmark");
    let all_measures = vec![
        XvaMeasure::Ee,
        XvaMeasure::Pfe,
        XvaMeasure::Cva,
        XvaMeasure::Dva,
        XvaMeasure::Fva,
        XvaMeasure::Mva,
        XvaMeasure::Kva,
    ];

    let shared_input = request(
        &context,
        cube(context.context_hash.clone(), 0, times, paths),
        all_measures.clone(),
        false,
    );
    let started = Instant::now();
    let shared = engine.calculate_xva(&shared_input)?;
    let shared_pipeline_ms = started.elapsed().as_secs_f64() * 1000.0;

    let started = Instant::now();
    for measure in &all_measures {
        let separate = request(
            &context,
            cube(context.context_hash.clone(), 0, times, paths),
            vec![*measure],
            false,
        );
        let _ = engine.calculate_xva(&separate)?;
    }
    let separate_measures_ms = started.elapsed().as_secs_f64() * 1000.0;

    let materialized_cube = cube(context.context_hash.clone(), 0, times, paths);
    let materialized_bytes_estimate = estimate_memory_bytes(&materialized_cube);
    let started = Instant::now();
    let materialized = engine.calculate_xva(&request(
        &context,
        materialized_cube,
        all_measures.clone(),
        true,
    ))?;
    let materialized_ms = started.elapsed().as_secs_f64() * 1000.0;

    let started = Instant::now();
    let mut streaming_peak_bytes_estimate = 0_u64;
    let mut cva_streaming = 0.0;
    let mut streaming_lineage = Vec::new();
    for offset in (0..times).step_by(chunk_size) {
        let rows = chunk_size.min(times - offset);
        // Include the prior boundary row in every non-first chunk. A zero discount on
        // that row prevents double counting while preserving the global hazard interval.
        let boundary = if offset > 0 { 1 } else { 0 };
        let mut chunk = cube(
            context.context_hash.clone(),
            offset.saturating_sub(boundary),
            rows + boundary,
            paths,
        );
        if offset > 0 {
            chunk.discount_factors = std::iter::once(0.0)
                .chain(std::iter::repeat_n(1.0, rows))
                .collect();
        }
        streaming_peak_bytes_estimate =
            streaming_peak_bytes_estimate.max(estimate_memory_bytes(&chunk));
        let result =
            engine.calculate_xva(&request(&context, chunk, all_measures.clone(), false))?;
        cva_streaming += result.cva;
        streaming_lineage.extend(result.provenance.artifact_lineage);
    }
    let streaming_ms = started.elapsed().as_secs_f64() * 1000.0;

    let output = BenchmarkResult {
        schema: "quant.xva-benchmark/v2",
        times,
        paths,
        chunk_size,
        shared_pipeline_ms,
        separate_measures_ms,
        materialized_ms,
        streaming_ms,
        materialized_bytes_estimate,
        streaming_peak_bytes_estimate,
        materialized_artifact_lineage: materialized.provenance.artifact_lineage,
        streaming_artifact_lineage: streaming_lineage,
        shared_graph: shared.provenance.shared_exposure_graph,
        arrow_output: "pending",
        cva_shared: shared.cva,
        cva_streaming,
        streaming_matches_materialized: (shared.cva - cva_streaming).abs() < 1e-12,
    };
    println!("{}", serde_json::to_string_pretty(&json!(output))?);
    Ok(())
}
