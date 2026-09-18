//! In-process companion to `benches/run_rest_overhead.py`.
//!
//! It isolates decode, queue/admission, compute and encode. HTTP is intentionally unavailable
//! here because this binary does not claim to be a network benchmark.
use quant_domain::{ContextCommand, MeasureSpec, PricingInput, QuantContext, ResourceRef};
use quant_engine::{Engine, EngineConfig};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    sync::{mpsc, Arc},
    thread,
    time::Instant,
};

#[derive(Default)]
struct Samples {
    decode: Vec<u128>,
    queue: Vec<u128>,
    compute: Vec<u128>,
    encode: Vec<u128>,
}

fn percentile(values: &[u128], p: f64) -> Option<u128> {
    if values.is_empty() {
        return None;
    }
    let mut sorted = values.to_vec();
    sorted.sort_unstable();
    let rank = ((sorted.len() as f64) * p).ceil() as usize;
    let index = rank.saturating_sub(1);
    sorted.get(index.min(sorted.len() - 1)).copied()
}

fn percentile_map(values: &[u128]) -> Value {
    json!({"p50": percentile(values, 0.50), "p95": percentile(values, 0.95), "p99": percentile(values, 0.99)})
}

fn request() -> PricingInput {
    let context = QuantContext::new("overhead").apply(&[
        ContextCommand::AddMarket { id: "m".into(), spec: json!({"r0":0.02}) },
        ContextCommand::AddModel { id: "hw".into(), spec: json!({"a":0.1,"b":0.03,"sigma":0.01}) },
        ContextCommand::AddProduct { id: "irs".into(), spec: json!({"notional":1_000_000.0,"fixed_rate":0.02,"payment_times":[1.0,2.0],"accruals":[1.0,1.0]}) },
    ]).unwrap().0;
    PricingInput {
        context,
        operation_id: "overhead".into(),
        market: Some(ResourceRef {
            id: "m".into(),
            hash: String::new(),
            kind: "market".into(),
            version: 1,
        }),
        model: Some(ResourceRef {
            id: "hw".into(),
            hash: String::new(),
            kind: "model".into(),
            version: 1,
        }),
        products: vec![ResourceRef {
            id: "irs".into(),
            hash: String::new(),
            kind: "product".into(),
            version: 1,
        }],
        measures: vec![MeasureSpec {
            name: "PV".into(),
            params: serde_json::Map::new(),
        }],
        pricing: BTreeMap::new(),
        execution: BTreeMap::new(),
        output: BTreeMap::new(),
    }
}

fn run_profile(wire: Arc<Vec<u8>>, iterations: usize, concurrency: usize) -> Value {
    let engine = Arc::new(
        Engine::new(EngineConfig {
            queue_capacity: 64,
            ..Default::default()
        })
        .unwrap(),
    );
    let (sender, receiver) = mpsc::channel();
    thread::scope(|scope| {
        for worker in 0..concurrency {
            let engine = Arc::clone(&engine);
            let wire = Arc::clone(&wire);
            let sender = sender.clone();
            scope.spawn(move || {
                let mut samples = Samples::default();
                for _ in (worker..iterations).step_by(concurrency) {
                    let decode_start = Instant::now();
                    let input: PricingInput = serde_json::from_slice(&wire).unwrap();
                    samples.decode.push(decode_start.elapsed().as_nanos());
                    let queue_start = Instant::now();
                    let handle = loop {
                        match engine.submit_price(input.clone()) {
                            Ok(handle) => break handle,
                            Err(quant_engine::QuantError::QueueFull) => thread::yield_now(),
                            Err(error) => panic!("benchmark admission failed: {error}"),
                        }
                    };
                    samples.queue.push(queue_start.elapsed().as_nanos());
                    let compute_start = Instant::now();
                    let result = handle.wait().unwrap();
                    samples.compute.push(compute_start.elapsed().as_nanos());
                    let encode_start = Instant::now();
                    let _encoded = serde_json::to_vec(&result).unwrap();
                    samples.encode.push(encode_start.elapsed().as_nanos());
                }
                sender.send(samples).unwrap();
            });
        }
    });
    drop(sender);
    let mut all = Samples::default();
    for samples in receiver {
        all.decode.extend(samples.decode);
        all.queue.extend(samples.queue);
        all.compute.extend(samples.compute);
        all.encode.extend(samples.encode);
    }
    json!({
        "concurrency": concurrency,
        "iterations": iterations,
        "completed": all.decode.len(),
        "percentiles_ns": {
            "decode": percentile_map(&all.decode),
            "queue": percentile_map(&all.queue),
            "compute": percentile_map(&all.compute),
            "encode": percentile_map(&all.encode),
        }
    })
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let iterations = args
        .iter()
        .position(|arg| arg == "--iterations")
        .and_then(|index| args.get(index + 1))
        .and_then(|value| value.parse().ok())
        .unwrap_or(20);
    let wire = Arc::new(serde_json::to_vec(&request()).unwrap());
    let saturation = std::thread::available_parallelism()
        .map(|value| value.get() * 2)
        .unwrap_or(16)
        .max(16);
    let profiles = [("1", 1), ("8", 8), ("saturation", saturation)]
        .into_iter()
        .map(|(name, concurrency)| {
            (
                name.to_string(),
                run_profile(Arc::clone(&wire), iterations, concurrency),
            )
        })
        .collect::<serde_json::Map<String, Value>>();
    println!(
        "{}",
        serde_json::to_string_pretty(&json!({
            "schema": "quant.rest-overhead/v1",
            "payload_bytes": wire.len(),
            "percentile_definition": "nearest rank over completed samples; nanoseconds",
            "profiles": profiles,
            "unavailable_stages": ["http_roundtrip"],
            "notes": ["No threshold or SLO is inferred from these observations."]
        }))
        .unwrap()
    );
}
