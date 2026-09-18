//! Local Fase 6 harness: admission/queue saturation only (not an SLO benchmark).
//!
//! Run with `cargo run --release -p quant-engine --example phase6_scheduler`. Hardware
//! counters are intentionally not fabricated; collect them externally with perf/ETW when
//! available.

use quant_engine::{AdmissionLimits, JobCost, WeightedAdmission};
use std::{
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

fn main() {
    let workers = std::thread::available_parallelism()
        .map(|value| value.get())
        .unwrap_or(1);
    println!("workers={workers} counters=unavailable");
    for cpu_units in [1, 2, 4, workers]
        .into_iter()
        .filter(|value| *value <= workers)
    {
        let admission = WeightedAdmission::new(AdmissionLimits::new(cpu_units as u32, 1 << 30));
        let start = Instant::now();
        let accepted = Arc::new(AtomicU64::new(0));
        let rejected = Arc::new(AtomicU64::new(0));
        thread::scope(|scope| {
            for index in 0..workers.saturating_mul(4) {
                let admission = Arc::clone(&admission);
                let accepted = Arc::clone(&accepted);
                let rejected = Arc::clone(&rejected);
                scope.spawn(move || {
                    let cost = if index % 10 == 0 {
                        JobCost::new(1, 32 << 20)
                    } else {
                        JobCost::new(1, 1 << 20)
                    };
                    for _ in 0..100 {
                        if let Some(permit) = admission.try_acquire(cost) {
                            accepted.fetch_add(1, Ordering::Relaxed);
                            thread::sleep(Duration::from_micros(50));
                            permit.release();
                        } else {
                            rejected.fetch_add(1, Ordering::Relaxed);
                        }
                    }
                });
            }
        });
        println!(
            "cpu_units={cpu_units} accepted={accepted} rejected={rejected} elapsed_ms={}",
            start.elapsed().as_secs_f64() * 1_000.0,
            accepted = accepted.load(Ordering::Relaxed),
            rejected = rejected.load(Ordering::Relaxed),
        );
    }
}
