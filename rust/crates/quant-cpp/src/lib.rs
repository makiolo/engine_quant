//! C++ legacy provider for the phase-2 Rust planner.
//!
//! The adapter owns one immutable C++ kernel. Numeric inputs are owned by the compiled plan and
//! borrowed only for the duration of a call; output is always allocated by the Rust registry.

mod bridge;

use bridge::ffi;
use quant_engine::{
    BatchItem, Capabilities, CompiledBatch, KernelDescriptor, KernelId, KernelInput,
    KernelRegistry, PlanRequirements, PlannerError, PricingKernel, ProviderId,
};
use std::sync::Arc;

pub use bridge::ffi::{BatchShape, CppErrorCode, CppStatus};

pub const BRIDGE_VERSION: u32 = 1;
pub const IRS_HULL_WHITE_KERNEL: u32 = 1;

#[derive(Debug, thiserror::Error)]
pub enum CppError {
    #[error("C++ bridge construction failed: {0}")]
    Construction(String),
    #[error("C++ bridge returned {code:?}: {message}")]
    Status { code: CppErrorCode, message: String },
}

/// Adapter implementing the phase-2 provider SPI. The leaf is immutable and has no thread-local
/// state, so sharing one owner across workers is safe under the explicit unsafe contract below.
pub struct CppKernelAdapter {
    kernel: cxx::UniquePtr<ffi::CppKernel>,
    descriptor: KernelDescriptor,
}

impl CppKernelAdapter {
    pub fn new_irs_hull_white() -> Result<Self, CppError> {
        let mut config = BRIDGE_VERSION.to_le_bytes().to_vec();
        config.extend_from_slice(&IRS_HULL_WHITE_KERNEL.to_le_bytes());
        let kernel = ffi::new_kernel(IRS_HULL_WHITE_KERNEL, &config)
            .map_err(|error| CppError::Construction(error.to_string()))?;
        if ffi::bridge_version() != BRIDGE_VERSION {
            return Err(CppError::Construction("bridge version mismatch".into()));
        }
        Ok(Self {
            kernel,
            descriptor: KernelDescriptor {
                id: KernelId::from("cpp-irs-hull-white-pv"),
                provider: ProviderId::from("cpp"),
                model_kinds: vec!["hull_white_1f".into()],
                product_kinds: vec!["ir_swap".into()],
                measures: vec!["PV".into()],
                capabilities: Capabilities(Capabilities::ANALYTIC.0 | Capabilities::CPU.0),
                input_layout_version: BRIDGE_VERSION,
                priority: 100,
            },
        })
    }

    pub fn capabilities(&self) -> u64 {
        self.kernel.capabilities()
    }

    pub fn register(registry: &mut KernelRegistry) -> Result<Arc<Self>, CppError> {
        let adapter = Arc::new(Self::new_irs_hull_white()?);
        registry.register(Arc::clone(&adapter) as Arc<dyn PricingKernel>);
        Ok(adapter)
    }

    /// Direct bridge harness used by integration tests and low-level benchmarks. Slices are
    /// borrowed only while this function is on the stack and `output` must have one slot per
    /// trade; the C++ owner never stores any of these pointers.
    #[allow(clippy::too_many_arguments)]
    pub fn price_batch(
        &self,
        shape: BatchShape,
        market: &[f64],
        model: &[f64],
        products: &[f64],
        payment_times: &[f64],
        accruals: &[f64],
        output: &mut [f64],
    ) -> Result<(), CppError> {
        let status = self.kernel.price_batch(
            shape,
            market,
            model,
            products,
            payment_times,
            accruals,
            output,
        );
        if status.code == CppErrorCode::Ok {
            Ok(())
        } else {
            Err(CppError::Status {
                code: status.code,
                message: status.message,
            })
        }
    }

    fn execute_inputs(
        &self,
        inputs: &[&KernelInput],
        output: &mut [f64],
    ) -> Result<(), PlannerError> {
        let first = inputs
            .first()
            .ok_or_else(|| PlannerError::InvalidBatch("C++ provider requires one input".into()))?;
        if inputs.iter().any(|input| {
            input.market != first.market
                || input.model != first.model
                || input.payment_times != first.payment_times
                || input.accruals != first.accruals
                || input.product.len() != 4
        }) {
            return Err(PlannerError::InvalidBatch(
                "C++ IRS batch must share market/model/schedule".into(),
            ));
        }
        let products = inputs
            .iter()
            .flat_map(|input| input.product.iter().copied())
            .collect::<Vec<_>>();
        let shape = BatchShape {
            trades: inputs.len() as u64,
            scenarios: 1,
            factors: 1,
            times: first.payment_times.len() as u64,
        };
        self.price_batch(
            shape,
            &first.market,
            &first.model,
            &products,
            &first.payment_times,
            &first.accruals,
            output,
        )
        .map_err(cpp_error_to_planner)
    }
}

fn cpp_error_to_planner(error: CppError) -> PlannerError {
    match error {
        CppError::Status { code, message } => PlannerError::Legacy {
            provider: "cpp".into(),
            code: error_code_number(code),
            message,
        },
        CppError::Construction(message) => PlannerError::Legacy {
            provider: "cpp".into(),
            code: error_code_number(CppErrorCode::Internal),
            message,
        },
    }
}

fn error_code_number(code: CppErrorCode) -> u32 {
    match code {
        CppErrorCode::Ok => 0,
        CppErrorCode::InvalidArgument => 1,
        CppErrorCode::Unsupported => 2,
        CppErrorCode::NumericalFailure => 3,
        CppErrorCode::ResourceExhausted => 4,
        CppErrorCode::Internal => 255,
        _ => 255,
    }
}

// SAFETY: CppKernel contains only an immutable kind id. `price_batch` is const, retains no
// borrowed Rust slices, and writes only to the caller-provided output. The C++ leaf has no global
// mutable state, so concurrent calls on one adapter are independent.
unsafe impl Send for CppKernelAdapter {}
unsafe impl Sync for CppKernelAdapter {}

impl PricingKernel for CppKernelAdapter {
    fn descriptor(&self) -> &KernelDescriptor {
        &self.descriptor
    }

    fn compile_batch(
        &self,
        request: &PlanRequirements,
        items: &[BatchItem],
        context_hash: &quant_domain::ContextHash,
    ) -> Result<CompiledBatch, PlannerError> {
        if items.iter().any(|item| item.input.is_none()) {
            return Err(PlannerError::InvalidBatch(
                "C++ provider needs a numeric market/model/product payload".into(),
            ));
        }
        Ok(CompiledBatch {
            kernel: self.descriptor.id.clone(),
            provider: self.descriptor.provider.clone(),
            context_hash: context_hash.clone(),
            requirements: request.clone(),
            items: items.to_vec(),
        })
    }

    fn execute(&self, batch: &CompiledBatch, output: &mut [f64]) -> Result<(), PlannerError> {
        if output.len() != batch.items.len() {
            return Err(PlannerError::InvalidBatch("output length mismatch".into()));
        }
        let inputs = batch
            .items
            .iter()
            .map(|item| item.input.as_ref())
            .collect::<Option<Vec<_>>>()
            .ok_or_else(|| PlannerError::InvalidBatch("C++ input payload missing".into()))?;
        self.execute_inputs(&inputs, output)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use quant_domain::{MarketSpec, ModelSpec, PricingContext, ProductSpec};
    use quant_engine::{Engine, EngineConfig, PlanRequest, PricingKernel, QuantError};

    fn request() -> PlanRequest {
        PlanRequest::inline(
            "m",
            MarketSpec::new(serde_json::json!({"r0": 0.02})).unwrap(),
            "hw",
            ModelSpec::from_value(serde_json::json!({"a": 0.1, "b": 0.03, "sigma": 0.01})).unwrap(),
            "irs",
            ProductSpec::from_value(serde_json::json!({"notional": 1_000_000.0, "fixed_rate": 0.02, "payment_times": [1.0, 2.0], "accruals": [1.0, 1.0]})).unwrap(),
            "PV",
            PricingContext::default(),
        )
    }

    #[test]
    fn bridge_version_and_capabilities_are_stable() {
        let adapter = CppKernelAdapter::new_irs_hull_white().unwrap();
        assert_eq!(BRIDGE_VERSION, 1);
        assert_ne!(adapter.capabilities(), 0);
        assert_eq!(adapter.descriptor().input_layout_version, BRIDGE_VERSION);
    }

    #[test]
    fn rust_plan_executes_real_cpp_irs_leaf() {
        let adapter = CppKernelAdapter::new_irs_hull_white().unwrap();
        let request = request();
        let mut registry = KernelRegistry::new();
        registry.register(Arc::new(adapter));
        let plan = registry.compile_plan(&request).unwrap();
        let values = registry.execute(&plan).unwrap();
        assert_eq!(values.len(), 1);
        assert!(values[0].is_finite());
    }

    #[test]
    fn cpp_leaf_matches_rust_irs_pv() {
        let adapter = CppKernelAdapter::new_irs_hull_white().unwrap();
        let mut output = [0.0];
        adapter
            .price_batch(
                BatchShape {
                    trades: 1,
                    scenarios: 1,
                    factors: 1,
                    times: 2,
                },
                &[0.02],
                &[0.1, 0.03, 0.01],
                &[1_000_000.0, 0.02, 0.0, 0.0],
                &[1.0, 2.0],
                &[1.0, 1.0],
                &mut output,
            )
            .unwrap();
        let rust = engine_core::api::irs_hull_white_npv(
            0.1,
            0.03,
            0.01,
            0.02,
            1_000_000.0,
            0.02,
            false,
            0.0,
            vec![1.0, 2.0],
            vec![1.0, 1.0],
        );
        assert!((output[0] - rust).abs() < 1e-8 * rust.abs().max(1.0));
    }

    #[test]
    fn cpp_leaf_matches_rust_par_rate_convention() {
        let adapter = CppKernelAdapter::new_irs_hull_white().unwrap();
        let mut output = [0.0];
        adapter
            .price_batch(
                BatchShape {
                    trades: 1,
                    scenarios: 1,
                    factors: 1,
                    times: 2,
                },
                &[0.02],
                &[0.1, 0.03, 0.01],
                &[1_000_000.0, 0.0, 0.0, 1.0],
                &[1.0, 2.0],
                &[1.0, 1.0],
                &mut output,
            )
            .unwrap();
        let rust = engine_core::api::irs_hull_white_npv(
            0.1,
            0.03,
            0.01,
            0.02,
            1_000_000.0,
            0.0,
            true,
            0.0,
            vec![1.0, 2.0],
            vec![1.0, 1.0],
        );
        assert!((output[0] - rust).abs() < 1e-8 * rust.abs().max(1.0));
    }

    #[test]
    fn concurrent_execution_is_safe() {
        let adapter = Arc::new(CppKernelAdapter::new_irs_hull_white().unwrap());
        let request = request();
        let mut registry = KernelRegistry::new();
        registry.register(Arc::clone(&adapter) as Arc<dyn PricingKernel>);
        let plan = registry.compile_plan(&request).unwrap();
        let handles = (0..4)
            .map(|_| {
                let adapter = Arc::clone(&adapter);
                let plan = plan.clone();
                std::thread::spawn(move || {
                    let mut output = vec![0.0];
                    adapter.execute(&plan.batches[0], &mut output).unwrap();
                    output[0]
                })
            })
            .collect::<Vec<_>>();
        let values = handles
            .into_iter()
            .map(|handle| handle.join().unwrap())
            .collect::<Vec<_>>();
        assert!(values
            .windows(2)
            .all(|window| (window[0] - window[1]).abs() < 1e-12));
    }

    #[test]
    fn empty_and_large_borrowed_slices_follow_shape_contract() {
        let adapter = CppKernelAdapter::new_irs_hull_white().unwrap();
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

        let trades = 2_048usize;
        let mut products = Vec::with_capacity(trades * 4);
        for _ in 0..trades {
            products.extend_from_slice(&[1_000_000.0, 0.02, 0.0, 0.0]);
        }
        let mut output = vec![0.0; trades];
        adapter
            .price_batch(
                BatchShape {
                    trades: trades as u64,
                    scenarios: 1,
                    factors: 1,
                    times: 2,
                },
                &[0.02],
                &[0.1, 0.03, 0.01],
                &products,
                &[1.0, 2.0],
                &[1.0, 1.0],
                &mut output,
            )
            .unwrap();
        assert!(output.iter().all(|value| value.is_finite()));
    }

    #[test]
    fn bridge_translates_shape_and_numerical_errors_without_panics() {
        let adapter = CppKernelAdapter::new_irs_hull_white().unwrap();
        let err = adapter
            .price_batch(
                BatchShape {
                    trades: 1,
                    scenarios: 2,
                    factors: 1,
                    times: 1,
                },
                &[0.02],
                &[0.1, 0.03, 0.01],
                &[1_000_000.0, 0.02, 0.0, 0.0],
                &[1.0],
                &[1.0],
                &mut [0.0],
            )
            .unwrap_err();
        assert!(matches!(
            err,
            CppError::Status {
                code: CppErrorCode::Unsupported,
                ..
            }
        ));

        let err = adapter
            .price_batch(
                BatchShape {
                    trades: 1,
                    scenarios: 1,
                    factors: 1,
                    times: 1,
                },
                &[0.02],
                &[0.0, 0.03, 0.01],
                &[1_000_000.0, 0.02, 0.0, 0.0],
                &[1.0],
                &[1.0],
                &mut [0.0],
            )
            .unwrap_err();
        assert!(matches!(
            err,
            CppError::Status {
                code: CppErrorCode::NumericalFailure,
                ..
            }
        ));
    }

    #[test]
    fn provider_error_maps_to_quant_error_legacy_category() {
        let adapter = Arc::new(CppKernelAdapter::new_irs_hull_white().unwrap());
        let engine = Engine::with_providers(EngineConfig::default(), vec![adapter]).unwrap();
        let request = PlanRequest::inline(
            "m",
            MarketSpec::new(serde_json::json!({"r0": 0.02})).unwrap(),
            "hw",
            ModelSpec::from_value(serde_json::json!({"a": 0.0, "b": 0.03, "sigma": 0.01})).unwrap(),
            "irs",
            ProductSpec::from_value(serde_json::json!({"notional": 1_000_000.0, "fixed_rate": 0.02, "payment_times": [1.0], "accruals": [1.0]})).unwrap(),
            "PV",
            PricingContext::default(),
        );
        let error = engine.price_typed(request).unwrap_err();
        assert!(matches!(error, QuantError::Legacy { code: 3, .. }));
    }
}
