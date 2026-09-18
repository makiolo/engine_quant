//! Small phase-5 harness.  It reports relative work and an explicit memory estimate; it does
//! not assert an SLO because machine and backend are deliberately not fixed in this phase.

use quant_domain::{
    BumpScheme, ContextCommand, Portfolio, ResourceRef, RiskFactor, RiskMeasure, RiskRequest, Trade,
};
use quant_engine::Engine;
use serde_json::json;
use std::time::Instant;

fn request(
    measures: Vec<RiskMeasure>,
    factors: Vec<RiskFactor>,
    bump_scheme: BumpScheme,
) -> RiskRequest {
    let context = quant_domain::QuantContext::new("phase5-harness")
        .apply(&[
            ContextCommand::AddMarket { id: "m".into(), spec: json!({"r0": 0.02}) },
            ContextCommand::AddModel { id: "hw".into(), spec: json!({"a": 0.1, "b": 0.03, "sigma": 0.01}) },
            ContextCommand::AddProduct { id: "irs".into(), spec: json!({"notional": 1_000_000.0, "fixed_rate": 0.02, "payment_times": [1.0,2.0,3.0,4.0,5.0], "accruals": [1.0,1.0,1.0,1.0,1.0]}) },
        ]).unwrap().0;
    let mut trade = Trade::new("t-1", "irs");
    trade.product = Some(context.product_handle("irs").unwrap().entry().spec.clone());
    let context = context
        .add_portfolio(Portfolio::new("book", vec![trade]))
        .unwrap();
    RiskRequest {
        context,
        operation_id: "phase5-harness".into(),
        portfolio_id: "book".into(),
        market: ResourceRef {
            id: "m".into(),
            hash: String::new(),
            kind: "market".into(),
            version: 1,
        },
        model: ResourceRef {
            id: "hw".into(),
            hash: String::new(),
            kind: "model".into(),
            version: 1,
        },
        measures,
        factors,
        bump_scheme,
        bump_size: Some(1e-4),
        tolerances: Default::default(),
        pricing: Default::default(),
    }
}

fn main() {
    let engine = Engine::new(Default::default()).expect("engine");
    let aad_request = request(
        vec![RiskMeasure::Delta, RiskMeasure::Rho, RiskMeasure::Vega],
        vec![],
        BumpScheme::Central,
    );
    let start = Instant::now();
    let aad = engine.calculate_risk(&aad_request).expect("AAD risk");
    let aad_elapsed = start.elapsed();

    let factors = vec![
        RiskFactor::curve_bucket("discount", 0),
        RiskFactor::curve_bucket("discount", 1),
        RiskFactor::curve_bucket("discount", 2),
        RiskFactor::curve_bucket("discount", 3),
        RiskFactor::curve_bucket("discount", 4),
    ];
    let start = Instant::now();
    let bumps = engine
        .calculate_risk(&request(
            vec![RiskMeasure::Rho],
            factors.clone(),
            BumpScheme::Central,
        ))
        .expect("bump risk");
    let bump_elapsed = start.elapsed();

    let start = Instant::now();
    for measure in [RiskMeasure::Delta, RiskMeasure::Rho, RiskMeasure::Vega] {
        engine
            .calculate_risk(&request(vec![measure], vec![], BumpScheme::Central))
            .expect("measure risk");
    }
    let separate_elapsed = start.elapsed();

    let matrix_bytes = aad.values.len() * std::mem::size_of::<f64>();
    // Burn's tape is backend-owned; this conservative estimate is only the scalar leaves plus
    // the dense output and is intentionally labelled as an estimate, not an allocator metric.
    let tape_estimate = aad
        .provenance
        .values()
        .map(|p| p.aad_passes)
        .max()
        .unwrap_or(0)
        * 4
        * std::mem::size_of::<f64>();
    println!(
        "phase5 aad_vs_n_bumps aad={:?} central_bumps={:?} bump_cells={}",
        aad_elapsed,
        bump_elapsed,
        bumps.values.len()
    );
    println!(
        "phase5 shared_graph measures={:?} separate_measure_calls={:?}",
        aad_elapsed, separate_elapsed
    );
    println!(
        "phase5 memory_estimate dense_matrix_bytes={} tape_scalar_estimate_bytes={} artifacts={}",
        matrix_bytes,
        tape_estimate,
        aad.artifacts.len()
    );
}
