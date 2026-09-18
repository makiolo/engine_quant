//! Phase-7 harness for the selected reduction kernel.
//!
//! Run in release mode for useful numbers:
//! `cargo run -p quant-engine --release --example phase7_simd_bench`
//!
//! The direct kernel compares the strict scalar reference, conservative production Auto and forced AVX2 over
//! aligned and offset (unaligned) slices.  The second section exercises the same reduction via
//! the end-to-end risk planner over a portfolio.  `CpuVectorPolicy::PortableSimd` and
//! `Avx512` intentionally report scalar today; they are included in the API to make that
//! fallback visible rather than claiming an implementation that is not present. Auto remains
//! scalar because the end-to-end gate is not closed; use explicit Avx2 for the microkernel probe.

use quant_domain::{
    BumpScheme, ContextCommand, CpuVectorPolicy, Portfolio, PricingContext, ResourceRef,
    RiskMeasure, RiskRequest, Trade,
};
use quant_engine::{sum_f64, CpuVectorCapabilities, Engine};
use serde_json::json;
use std::time::{Duration, Instant};

#[derive(Clone, Copy)]
struct AoSValue {
    value: f64,
}

fn timed_sum(values: &[f64], policy: CpuVectorPolicy, rounds: usize) -> (Duration, f64) {
    let start = Instant::now();
    let mut value = 0.0;
    for _ in 0..rounds {
        value += sum_f64(values, &policy);
    }
    (start.elapsed(), value)
}

fn timed_aos(values: &[AoSValue], rounds: usize) -> (Duration, f64) {
    let start = Instant::now();
    let mut value = 0.0;
    for _ in 0..rounds {
        // This is deliberately the AoS baseline; the compiler may auto-vectorize it in release,
        // but it cannot use the contiguous SoA AVX2 kernel without a gather/copy.
        value += values.iter().map(|item| item.value).sum::<f64>();
    }
    (start.elapsed(), value)
}

fn request(portfolio: Portfolio, vector: CpuVectorPolicy) -> RiskRequest {
    let context = quant_domain::QuantContext::new("phase7-harness")
        .apply(&[
            ContextCommand::AddMarket { id: "m".into(), spec: json!({"r0": 0.02}) },
            ContextCommand::AddModel { id: "hw".into(), spec: json!({"a": 0.1, "b": 0.03, "sigma": 0.01}) },
            ContextCommand::AddProduct { id: "irs".into(), spec: json!({"notional": 1_000_000.0, "fixed_rate": 0.02, "payment_times": [1.0,2.0,3.0,4.0,5.0], "accruals": [1.0,1.0,1.0,1.0,1.0]}) },
        ]).unwrap().0
        .add_portfolio(portfolio).unwrap();
    RiskRequest {
        context,
        operation_id: "phase7-harness".into(),
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
        measures: vec![RiskMeasure::Pv],
        factors: Vec::new(),
        bump_scheme: BumpScheme::Central,
        bump_size: None,
        tolerances: Default::default(),
        pricing: PricingContext {
            execution: quant_domain::ExecutionPolicy {
                cpu_vector: vector,
                ..Default::default()
            },
            ..Default::default()
        },
    }
}

fn main() {
    let n = 1 << 16;
    let rounds = 128;
    let aligned: Vec<f64> = (0..n).map(|i| 1.0 + (i % 17) as f64 * 1e-6).collect();
    let storage: Vec<f64> = std::iter::once(-1.0)
        .chain(aligned.iter().copied())
        .collect();
    let unaligned = &storage[1..];
    let aos: Vec<AoSValue> = aligned
        .iter()
        .copied()
        .map(|value| AoSValue { value })
        .collect();
    let caps = CpuVectorCapabilities::detect();
    println!("phase7 capabilities={caps:?} n={n} rounds={rounds}");
    for (name, values) in [("aligned", aligned.as_slice()), ("unaligned", unaligned)] {
        for policy in [
            CpuVectorPolicy::Scalar,
            CpuVectorPolicy::Auto,
            CpuVectorPolicy::Avx2,
        ] {
            let (elapsed, checksum) = timed_sum(values, policy.clone(), rounds);
            println!("phase7 kernel layout={name} policy={policy:?} elapsed={elapsed:?} checksum={checksum:.6}");
        }
    }
    let (aos_elapsed, aos_checksum) = timed_aos(&aos, rounds);
    println!("phase7 kernel layout=aos policy=Scalar(auto-vectorizer) elapsed={aos_elapsed:?} checksum={aos_checksum:.6}");

    let engine = Engine::new(Default::default()).expect("engine");
    let product = quant_domain::ProductSpec::from_value(json!({
        "notional": 1_000_000.0, "fixed_rate": 0.02,
        "payment_times": [1.0,2.0,3.0,4.0,5.0],
        "accruals": [1.0,1.0,1.0,1.0,1.0]
    }))
    .expect("product");
    let mut trades = Vec::with_capacity(n);
    for i in 0..n {
        let mut trade = Trade::new(format!("t-{i}"), "irs");
        trade.quantity = 1.0 + (i % 17) as f64 * 1e-6;
        trade.product = Some(product.clone());
        trades.push(trade);
    }
    let portfolio = Portfolio::new("book", trades);
    for policy in [
        CpuVectorPolicy::Scalar,
        CpuVectorPolicy::Auto,
        CpuVectorPolicy::Avx2,
    ] {
        let req = request(portfolio.clone(), policy.clone());
        let start = Instant::now();
        let result = engine.calculate_risk(&req).expect("risk");
        println!(
            "phase7 end_to_end policy={policy:?} elapsed={:?} base_value={:.6}",
            start.elapsed(),
            result.base_value
        );
    }
}
