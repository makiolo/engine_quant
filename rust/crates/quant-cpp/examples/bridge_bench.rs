//! Small repeatable bridge harness. It reports timings only; no SLO is inferred from a laptop.
//! Run with `cargo run -p quant-cpp --release --example bridge_bench`.

use quant_cpp::{BatchShape, CppKernelAdapter};
use std::time::{Duration, Instant};

fn timed(mut call: impl FnMut()) -> Duration {
    let start = Instant::now();
    for _ in 0..200 {
        call();
    }
    start.elapsed() / 200
}

fn main() {
    let adapter = CppKernelAdapter::new_irs_hull_white().expect("bridge must construct");
    let market = [0.02];
    let model = [0.1, 0.03, 0.01];
    let times = [1.0, 2.0, 3.0, 4.0, 5.0];
    let accruals = [1.0; 5];
    let scalar_product = [1_000_000.0, 0.02, 0.0, 0.0];

    let no_op = timed(|| {
        adapter
            .price_batch(
                BatchShape {
                    trades: 0,
                    scenarios: 1,
                    factors: 1,
                    times: 0,
                },
                &[],
                &[],
                &[],
                &[],
                &[],
                &mut [],
            )
            .unwrap();
    });
    let scalar = timed(|| {
        let mut output = [0.0];
        adapter
            .price_batch(
                BatchShape {
                    trades: 1,
                    scenarios: 1,
                    factors: 1,
                    times: 5,
                },
                &market,
                &model,
                &scalar_product,
                &times,
                &accruals,
                &mut output,
            )
            .unwrap();
        std::hint::black_box(output);
    });

    for &trades in &[8usize, 64, 512] {
        let borrowed_products = (0..trades).flat_map(|_| scalar_product).collect::<Vec<_>>();
        let mut output = vec![0.0; trades];
        let borrowed = timed(|| {
            adapter
                .price_batch(
                    BatchShape {
                        trades: trades as u64,
                        scenarios: 1,
                        factors: 1,
                        times: 5,
                    },
                    &market,
                    &model,
                    &borrowed_products,
                    &times,
                    &accruals,
                    &mut output,
                )
                .unwrap();
            std::hint::black_box(&output);
        });
        // Deliberately copy each input into owned vectors to make the copy-vs-borrow cost visible.
        let copied = timed(|| {
            let market_copy = market.to_vec();
            let model_copy = model.to_vec();
            let products_copy = borrowed_products.to_vec();
            let times_copy = times.to_vec();
            let accruals_copy = accruals.to_vec();
            let mut output_copy = vec![0.0; trades];
            adapter
                .price_batch(
                    BatchShape {
                        trades: trades as u64,
                        scenarios: 1,
                        factors: 1,
                        times: 5,
                    },
                    &market_copy,
                    &model_copy,
                    &products_copy,
                    &times_copy,
                    &accruals_copy,
                    &mut output_copy,
                )
                .unwrap();
            std::hint::black_box(output_copy);
        });
        // The extra `to_vec` below is an explicit C++-output+copy proxy. The production adapter
        // uses the Rust-preallocated output path and therefore does not pay this allocation.
        let output_copy = timed(|| {
            let mut cpp_output = vec![0.0; trades];
            adapter
                .price_batch(
                    BatchShape {
                        trades: trades as u64,
                        scenarios: 1,
                        factors: 1,
                        times: 5,
                    },
                    &market,
                    &model,
                    &borrowed_products,
                    &times,
                    &accruals,
                    &mut cpp_output,
                )
                .unwrap();
            let copied_result = cpp_output.to_vec();
            std::hint::black_box(copied_result);
        });
        println!(
            "batch={trades:4} borrowed={borrowed:?} copied_inputs={copied:?} rust_prealloc={borrowed:?} cpp_output_copy={output_copy:?}"
        );
    }
    println!("no-op={no_op:?} scalar={scalar:?}");
}
