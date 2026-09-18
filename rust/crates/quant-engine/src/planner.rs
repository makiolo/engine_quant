//! Provider registry and deterministic, batch-oriented planning (phase 2).

use quant_domain::{
    hash_value, ContextHash, ExecutionPolicy, MarketHandle, MarketSnapshot, MarketSpec,
    ModelHandle, ModelSpec, PricingContext, ProductHandle, ProductSpec,
};
use serde::{Deserialize, Serialize};
use std::{
    cmp::Ordering,
    collections::BTreeMap,
    sync::{
        atomic::{AtomicUsize, Ordering as AtomicOrdering},
        Arc, Mutex,
    },
};
use thiserror::Error;

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ProviderId(pub String);
impl From<&str> for ProviderId {
    fn from(v: &str) -> Self {
        Self(v.to_owned())
    }
}
impl From<String> for ProviderId {
    fn from(v: String) -> Self {
        Self(v)
    }
}
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct KernelId(pub String);
impl From<&str> for KernelId {
    fn from(v: &str) -> Self {
        Self(v.to_owned())
    }
}
impl From<String> for KernelId {
    fn from(v: String) -> Self {
        Self(v)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Capabilities(pub u64);
impl Capabilities {
    pub const ANALYTIC: Self = Self(1 << 0);
    pub const MONTE_CARLO: Self = Self(1 << 1);
    pub const PATH_DEPENDENT: Self = Self(1 << 2);
    pub const AAD: Self = Self(1 << 3);
    pub const CPU: Self = Self(1 << 4);
    pub const GPU: Self = Self(1 << 5);
    pub const SCENARIOS: Self = Self(1 << 6);
    pub const fn empty() -> Self {
        Self(0)
    }
    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}
impl Default for Capabilities {
    fn default() -> Self {
        Self::empty()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct KernelDescriptor {
    pub id: KernelId,
    pub provider: ProviderId,
    #[serde(default)]
    pub model_kinds: Vec<String>,
    #[serde(default)]
    pub product_kinds: Vec<String>,
    #[serde(default)]
    pub measures: Vec<String>,
    #[serde(default)]
    pub capabilities: Capabilities,
    #[serde(default)]
    pub input_layout_version: u32,
    #[serde(default)]
    pub priority: u32,
}
impl KernelDescriptor {
    pub fn supports(&self, request: &PlanRequirements) -> bool {
        (self.model_kinds.is_empty() || self.model_kinds.iter().any(|v| v == &request.model_kind))
            && (self.product_kinds.is_empty()
                || self
                    .product_kinds
                    .iter()
                    .any(|v| v == &request.product_kind))
            && (self.measures.is_empty()
                || self
                    .measures
                    .iter()
                    .any(|v| v.eq_ignore_ascii_case(&request.measure)))
            && self.capabilities.contains(request.required_capabilities)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlanRequirements {
    pub model_kind: String,
    pub product_kind: String,
    pub measure: String,
    #[serde(default)]
    pub required_capabilities: Capabilities,
    #[serde(default)]
    pub execution: ExecutionPolicy,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BatchItem {
    pub product_id: String,
    pub product_hash: ContextHash,
    pub model_hash: ContextHash,
    pub market_hash: ContextHash,
    /// Numeric payload prepared during planning.  The provider receives these vectors by
    /// borrow during execution; they are deliberately not encoded as JSON on the hot path.
    #[serde(default)]
    pub input: Option<KernelInput>,
}

/// Stable numeric layout shared by providers which opt into the phase-3 bridge contract.
/// `market = [r0]`, `model = [a, b, sigma]`, and `product = [notional, fixed_rate, start,
/// use_par_rate]`.  Schedules are row-major parallel arrays.  Providers which do not need a
/// numeric payload may leave `BatchItem::input` as `None`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct KernelInput {
    pub market: Vec<f64>,
    pub model: Vec<f64>,
    pub product: Vec<f64>,
    pub payment_times: Vec<f64>,
    pub accruals: Vec<f64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CompiledBatch {
    pub kernel: KernelId,
    pub provider: ProviderId,
    pub context_hash: ContextHash,
    pub requirements: PlanRequirements,
    pub items: Vec<BatchItem>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ExecutionPlan {
    pub batches: Vec<CompiledBatch>,
    pub fingerprint: ContextHash,
}

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum PlannerError {
    #[error("no pricing kernel supports {model_kind}/{product_kind}/{measure}")]
    NoCompatibleKernel {
        model_kind: String,
        product_kind: String,
        measure: String,
    },
    #[error("kernel returned an invalid batch: {0}")]
    InvalidBatch(String),
    #[error("legacy backend {provider}: {code}: {message}")]
    Legacy {
        provider: String,
        code: u32,
        message: String,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlanCacheStats {
    pub hits: usize,
    pub misses: usize,
}

#[derive(Debug, Clone)]
pub struct PlanRequest {
    pub market: MarketHandle,
    pub model: ModelHandle,
    pub products: Vec<ProductHandle>,
    pub measure: String,
    pub context: PricingContext,
    pub context_hash: ContextHash,
}
impl PlanRequest {
    pub fn single(
        market: MarketHandle,
        model: ModelHandle,
        product: ProductHandle,
        context: PricingContext,
    ) -> Self {
        let context_hash = hash_value(
            &serde_json::json!({"market": market.hash(), "model": model.hash(), "product": product.hash(), "context": context}),
        );
        Self {
            market,
            model,
            products: vec![product],
            measure: "PV".to_owned(),
            context,
            context_hash,
        }
    }

    pub fn from_context(
        context: &quant_domain::QuantContext,
        market_id: impl AsRef<str>,
        model_id: impl AsRef<str>,
        product_ids: &[impl AsRef<str>],
        measure: impl Into<String>,
        pricing: PricingContext,
    ) -> Result<Self, PlannerError> {
        let market = context
            .market_handle(market_id)
            .map_err(|e| PlannerError::InvalidBatch(e.to_string()))?;
        let model = context
            .model_handle(model_id)
            .map_err(|e| PlannerError::InvalidBatch(e.to_string()))?;
        let products = product_ids
            .iter()
            .map(|id| context.product_handle(id))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|e| PlannerError::InvalidBatch(e.to_string()))?;
        let mut request = Self {
            market,
            model,
            products,
            measure: measure.into(),
            context: pricing,
            context_hash: ContextHash(String::new()),
        };
        request.context_hash = request.semantic_hash();
        Ok(request)
    }

    #[allow(clippy::too_many_arguments)]
    pub fn inline(
        market_id: impl Into<String>,
        market: MarketSpec,
        model_id: impl Into<String>,
        model: ModelSpec,
        product_id: impl Into<String>,
        product: ProductSpec,
        measure: impl Into<String>,
        context: PricingContext,
    ) -> Self {
        let market_id = market_id.into();
        let market = MarketHandle::new(MarketSnapshot::new(market_id, market, 0, None));
        let model_hash = model.hash();
        let product_hash = product.hash();
        let model = ModelHandle::new(quant_domain::ModelEntry {
            id: model_id.into().into(),
            spec: model,
            version: 0,
            hash: model_hash,
            parent_hash: None,
        });
        let product = ProductHandle::new(quant_domain::ProductEntry {
            id: product_id.into().into(),
            spec: product,
            version: 0,
            hash: product_hash,
            parent_hash: None,
        });
        let mut request = Self::single(market, model, product, context);
        request.measure = measure.into();
        request.context_hash = request.semantic_hash();
        request
    }

    fn semantic_hash(&self) -> ContextHash {
        hash_value(&serde_json::json!({
            "market": self.market.hash(), "model": self.model.hash(),
            "products": self.products.iter().map(|p| p.hash()).collect::<Vec<_>>(),
            "measure": self.measure, "context": self.context,
        }))
    }
}

pub trait PricingKernel: Send + Sync {
    fn descriptor(&self) -> &KernelDescriptor;
    /// Validate/compile happens once per homogeneous group, before entering the hot path.
    fn compile_batch(
        &self,
        request: &PlanRequirements,
        items: &[BatchItem],
        context_hash: &ContextHash,
    ) -> Result<CompiledBatch, PlannerError> {
        Ok(CompiledBatch {
            kernel: self.descriptor().id.clone(),
            provider: self.descriptor().provider.clone(),
            context_hash: context_hash.clone(),
            requirements: request.clone(),
            items: items.to_vec(),
        })
    }
    fn execute(&self, batch: &CompiledBatch, output: &mut [f64]) -> Result<(), PlannerError>;
}

#[derive(Clone)]
pub struct KernelRegistry {
    kernels: Vec<Arc<dyn PricingKernel>>,
    cache: Arc<Mutex<BTreeMap<String, ExecutionPlan>>>,
    cache_hits: Arc<AtomicUsize>,
    cache_misses: Arc<AtomicUsize>,
}
impl Default for KernelRegistry {
    fn default() -> Self {
        Self {
            kernels: Vec::new(),
            cache: Arc::new(Mutex::new(BTreeMap::new())),
            cache_hits: Arc::new(AtomicUsize::new(0)),
            cache_misses: Arc::new(AtomicUsize::new(0)),
        }
    }
}
impl KernelRegistry {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn register(&mut self, kernel: Arc<dyn PricingKernel>) {
        self.kernels.push(kernel);
        self.kernels
            .sort_by(|a, b| descriptor_order(a.descriptor(), b.descriptor()));
        self.cache.lock().expect("plan cache lock").clear();
    }
    pub fn providers(&self) -> usize {
        self.kernels.len()
    }
    pub fn compile_plan(&self, request: &PlanRequest) -> Result<ExecutionPlan, PlannerError> {
        self.compile_plan_bounded(request, usize::MAX)
    }

    /// Compile a stable, heterogeneous plan without materialising a trade × scenario grid.
    /// Products are grouped by compatible product family and split into bounded batches.
    pub fn compile_plan_bounded(
        &self,
        request: &PlanRequest,
        max_batch_items: usize,
    ) -> Result<ExecutionPlan, PlannerError> {
        if max_batch_items == 0 {
            return Err(PlannerError::InvalidBatch(
                "max_batch_items must be positive".into(),
            ));
        }
        let key = format!("{}:{max_batch_items}", request.semantic_hash().0);
        if let Some(plan) = self
            .cache
            .lock()
            .expect("plan cache lock")
            .get(&key)
            .cloned()
        {
            self.cache_hits.fetch_add(1, AtomicOrdering::Relaxed);
            return Ok(plan);
        }
        self.cache_misses.fetch_add(1, AtomicOrdering::Relaxed);
        let requirement = PlanRequirements {
            model_kind: request.model.entry().spec.kind().0,
            product_kind: request
                .products
                .first()
                .map(|p| p.entry().spec.kind().0.clone())
                .unwrap_or_default(),
            measure: request.measure.clone(),
            required_capabilities: Capabilities::empty(),
            execution: request.context.execution.clone(),
        };
        // Keep first-seen family order so output order can be reconstructed deterministically.
        let mut groups: Vec<(String, Vec<&ProductHandle>)> = Vec::new();
        for product in &request.products {
            let family = compatibility_layout_key(product);
            if let Some((_, members)) = groups.iter_mut().find(|(kind, _)| *kind == family) {
                members.push(product);
            } else {
                groups.push((family, vec![product]));
            }
        }
        let mut batches = Vec::new();
        for (_family, members) in groups {
            let requirement = PlanRequirements {
                product_kind: members[0].entry().spec.kind().0,
                ..requirement.clone()
            };
            let kernel = self
                .kernels
                .iter()
                .find(|k| k.descriptor().supports(&requirement))
                .ok_or_else(|| PlannerError::NoCompatibleKernel {
                    model_kind: requirement.model_kind.clone(),
                    product_kind: requirement.product_kind.clone(),
                    measure: requirement.measure.clone(),
                })?;
            for chunk in members.chunks(max_batch_items) {
                let items = chunk
                    .iter()
                    .map(|product| BatchItem {
                        product_id: product.id().0.clone(),
                        product_hash: product.hash().clone(),
                        model_hash: request.model.hash().clone(),
                        market_hash: request.market.hash().clone(),
                        input: numeric_input(request, product),
                    })
                    .collect::<Vec<_>>();
                batches.push(kernel.compile_batch(&requirement, &items, &request.context_hash)?);
            }
        }
        let fingerprint = hash_value(&serde_json::to_value(&batches).expect("plan serializable"));
        let plan = ExecutionPlan {
            batches,
            fingerprint,
        };
        self.cache
            .lock()
            .expect("plan cache lock")
            .insert(key, plan.clone());
        Ok(plan)
    }
    pub fn cache_stats(&self) -> PlanCacheStats {
        PlanCacheStats {
            hits: self.cache_hits.load(AtomicOrdering::Relaxed),
            misses: self.cache_misses.load(AtomicOrdering::Relaxed),
        }
    }
    pub fn clear_cache(&self) {
        self.cache.lock().expect("plan cache lock").clear();
    }
    pub fn execute(&self, plan: &ExecutionPlan) -> Result<Vec<f64>, PlannerError> {
        let mut output = Vec::new();
        for batch in &plan.batches {
            let kernel = self
                .kernels
                .iter()
                .find(|k| k.descriptor().id == batch.kernel)
                .ok_or_else(|| PlannerError::InvalidBatch("kernel not registered".into()))?;
            let offset = output.len();
            output.resize(offset + batch.items.len(), 0.0);
            kernel.execute(batch, &mut output[offset..])?;
        }
        Ok(output)
    }
}

/// Product family used by capability matching.
pub fn compatibility_group_key(product: &ProductHandle) -> String {
    product.entry().spec.kind().0
}

/// Products can share a provider call only when their family and schedule layout agree. Price
/// parameters (notional/fixed rate) intentionally do not enter this key.
pub fn compatibility_layout_key(product: &ProductHandle) -> String {
    let kind = compatibility_group_key(product);
    let layout = match &product.entry().spec {
        ProductSpec::IrSwap(spec) => serde_json::json!({
            "payment_times": spec.params.get("payment_times"),
            "accruals": spec.params.get("accruals"),
        }),
        _ => serde_json::json!({}),
    };
    format!(
        "{kind}:{}",
        serde_json::to_string(&layout).expect("layout serializable")
    )
}

fn numeric_input(request: &PlanRequest, product: &ProductHandle) -> Option<KernelInput> {
    let market = match request.market.snapshot().spec.params.get("r0") {
        Some(value) => value.as_f64()?,
        None => 0.02,
    };
    let model_params = match &request.model.entry().spec {
        ModelSpec::HullWhite1F(spec) => &spec.params,
        ModelSpec::Heston(spec) => &spec.params,
        ModelSpec::Sabr(spec) => &spec.params,
        ModelSpec::Json { params, .. } => params,
        ModelSpec::External { .. } => return None,
    };
    let model = vec![
        model_params
            .get("a")
            .and_then(|v| v.as_f64())
            .unwrap_or(0.1),
        model_params
            .get("b")
            .and_then(|v| v.as_f64())
            .unwrap_or(0.03),
        model_params
            .get("sigma")
            .and_then(|v| v.as_f64())
            .unwrap_or(0.01),
    ];
    let product_params = match &product.entry().spec {
        ProductSpec::IrSwap(spec) => &spec.params,
        ProductSpec::EuropeanOption(spec) => &spec.params,
        ProductSpec::Json { params, .. } => params,
        ProductSpec::External { .. } => return None,
    };
    let payment_times = product_params
        .get("payment_times")?
        .as_array()?
        .iter()
        .map(serde_json::Value::as_f64)
        .collect::<Option<Vec<_>>>()?;
    let accruals = product_params
        .get("accruals")?
        .as_array()?
        .iter()
        .map(serde_json::Value::as_f64)
        .collect::<Option<Vec<_>>>()?;
    Some(KernelInput {
        market: vec![market],
        model,
        product: vec![
            product_params
                .get("notional")
                .and_then(|v| v.as_f64())
                .unwrap_or(1_000_000.0),
            product_params
                .get("fixed_rate")
                .and_then(|v| v.as_f64())
                .unwrap_or(0.0),
            product_params
                .get("start")
                .and_then(|v| v.as_f64())
                .unwrap_or(0.0),
            if product_params
                .get("use_par_rate")
                .and_then(serde_json::Value::as_bool)
                .unwrap_or(false)
            {
                1.0
            } else {
                0.0
            },
        ],
        payment_times,
        accruals,
    })
}

fn descriptor_order(a: &KernelDescriptor, b: &KernelDescriptor) -> Ordering {
    a.priority
        .cmp(&b.priority)
        .then_with(|| a.provider.cmp(&b.provider))
        .then_with(|| a.id.cmp(&b.id))
}

/// A tiny built-in kernel useful for smoke tests and for the existing phase-1 PV path.
pub struct ConstantKernel {
    descriptor: KernelDescriptor,
    value: f64,
}

/// In-process Rust provider for the stable IRS/Hull-White layout.  It has the same SPI as the
/// optional C++ provider, so the planner can select either implementation without changing the
/// portfolio API.
pub struct RustIrsKernel {
    descriptor: KernelDescriptor,
}
impl RustIrsKernel {
    pub fn new() -> Self {
        Self {
            descriptor: KernelDescriptor {
                id: KernelId::from("rust-irs-hull-white"),
                provider: ProviderId::from("rust"),
                model_kinds: vec!["hull_white_1f".into()],
                product_kinds: vec!["ir_swap".into()],
                measures: vec!["PV".into()],
                capabilities: Capabilities(Capabilities::ANALYTIC.0 | Capabilities::CPU.0),
                input_layout_version: 1,
                priority: 0,
            },
        }
    }
}
impl Default for RustIrsKernel {
    fn default() -> Self {
        Self::new()
    }
}
impl PricingKernel for RustIrsKernel {
    fn descriptor(&self) -> &KernelDescriptor {
        &self.descriptor
    }
    fn execute(&self, batch: &CompiledBatch, output: &mut [f64]) -> Result<(), PlannerError> {
        if output.len() != batch.items.len() {
            return Err(PlannerError::InvalidBatch("output length mismatch".into()));
        }
        for (index, item) in batch.items.iter().enumerate() {
            let input = item
                .input
                .as_ref()
                .ok_or_else(|| PlannerError::InvalidBatch("Rust IRS input missing".into()))?;
            if input.market.len() != 1 || input.model.len() < 3 || input.product.len() < 4 {
                return Err(PlannerError::InvalidBatch(
                    "Rust IRS input layout mismatch".into(),
                ));
            }
            output[index] = engine_core::api::irs_hull_white_npv(
                input.model[0],
                input.model[1],
                input.model[2],
                input.market[0],
                input.product[0],
                input.product[1],
                input.product[3] != 0.0,
                input.product[2],
                input.payment_times.clone(),
                input.accruals.clone(),
            );
        }
        Ok(())
    }
}
impl ConstantKernel {
    pub fn new(descriptor: KernelDescriptor, value: f64) -> Self {
        Self { descriptor, value }
    }
}
impl PricingKernel for ConstantKernel {
    fn descriptor(&self) -> &KernelDescriptor {
        &self.descriptor
    }
    fn execute(&self, batch: &CompiledBatch, output: &mut [f64]) -> Result<(), PlannerError> {
        if output.len() != batch.items.len() {
            return Err(PlannerError::InvalidBatch("output length mismatch".into()));
        }
        output.fill(self.value);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use quant_domain::{MarketSpec, ModelSpec, ProductSpec};

    fn descriptor() -> KernelDescriptor {
        KernelDescriptor {
            id: KernelId::from("hw-rust"),
            provider: ProviderId::from("rust"),
            model_kinds: vec!["hull_white_1f".into()],
            product_kinds: vec!["ir_swap".into()],
            measures: vec!["PV".into()],
            capabilities: Capabilities::empty(),
            input_layout_version: 1,
            priority: 0,
        }
    }

    fn request() -> PlanRequest {
        PlanRequest::inline(
            "m",
            MarketSpec::new(serde_json::json!({"r0": 0.02})).unwrap(),
            "hw",
            ModelSpec::from_value(serde_json::json!({"a": 0.1, "sigma": 0.01})).unwrap(),
            "irs",
            ProductSpec::from_value(serde_json::json!({"payment_times": [1.0], "accruals": [1.0]}))
                .unwrap(),
            "PV",
            PricingContext::default(),
        )
    }

    #[test]
    fn planner_is_deterministic_and_executes_one_item_batch() {
        let mut registry = KernelRegistry::new();
        registry.register(Arc::new(ConstantKernel::new(descriptor(), 42.0)));
        let request = request();
        let left = registry.compile_plan(&request).unwrap();
        let right = registry.compile_plan(&request).unwrap();
        assert_eq!(left.fingerprint, right.fingerprint);
        assert_eq!(
            registry.cache_stats(),
            PlanCacheStats { hits: 1, misses: 1 }
        );
        assert_eq!(registry.execute(&left).unwrap(), vec![42.0]);
    }

    #[test]
    fn incompatible_model_fails_before_execution() {
        let mut registry = KernelRegistry::new();
        registry.register(Arc::new(ConstantKernel::new(descriptor(), 42.0)));
        let mut request = request();
        request.model = PlanRequest::inline(
            "m",
            MarketSpec::new(serde_json::json!({"r0": 0.02})).unwrap(),
            "h",
            ModelSpec::from_value(serde_json::json!({"kind": "heston"})).unwrap(),
            "p",
            ProductSpec::from_value(serde_json::json!({"payment_times": [1.0]})).unwrap(),
            "PV",
            PricingContext::default(),
        )
        .model;
        assert!(matches!(
            registry.compile_plan(&request),
            Err(PlannerError::NoCompatibleKernel { .. })
        ));
    }
}
