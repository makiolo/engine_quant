//! Aplicación de cálculo y scheduler CPU acotado.
//!
//! Los workers son hilos dedicados: Tokio solo coordina la espera en el adaptador HTTP.
//! El scheduler no mantiene sesiones ni caches de contexto entre requests.

use quant_domain::{
    hash_value, ContextCommand, DomainError, ExposureCubeInput, MarketHandle, MarketSnapshot,
    ModelHandle, PartialFailure, Portfolio, PricingContext, PricingInput, PricingOutput,
    ProductHandle, QuantContext, ResourceRef, ScenarioSet, Trade, XvaRequest, XvaResult,
};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc, Arc, Mutex,
    },
    thread,
};
use thiserror::Error;

pub mod gpu;
mod planner;
mod risk;
pub mod scheduler;
pub mod simd;
pub mod xva;
pub use gpu::{
    GpuDeviceInfo, GpuDeviceQueueConfig, GpuError, GpuFallbackPolicy, GpuInventory,
    GpuMetricSource, GpuMetricValue, GpuMetrics, GpuMetricsSnapshot, GpuPermit, GpuPrecision,
    GpuProbe, GpuQueueError, GpuQueueSnapshot, GpuRouteDecision, GpuRouteReason, GpuRouteTarget,
    GpuRouter, GpuRoutingPolicy, GpuScheduler, GpuTimings, GpuWorkload,
};
pub use planner::{
    compatibility_group_key, compatibility_layout_key, BatchItem, Capabilities, CompiledBatch,
    ConstantKernel, ExecutionPlan, KernelDescriptor, KernelId, KernelInput, KernelRegistry,
    PlanCacheStats, PlanRequest, PlanRequirements, PlannerError, PricingKernel, ProviderId,
    RustIrsKernel,
};
pub use scheduler::{
    AdmissionLimits, AdmissionPermit, AdmissionSnapshot, JobCost, NestedParallelism,
    WeightedAdmission,
};
pub use simd::{
    resolve_cpu_vector_policy, resolve_cpu_vector_policy_with_auto_threshold, sum_f64,
    sum_f64_with_capabilities, CpuVectorCapabilities, CpuVectorStrategy,
};
pub use xva::estimate_memory_bytes;

/// Version de la aplicacion compartida con el crate numerico interno.
pub const VERSION: &str = engine_core::VERSION;

#[derive(Debug, Clone)]
pub struct EngineConfig {
    pub worker_count: usize,
    pub queue_capacity: usize,
    pub max_products: usize,
    pub max_payment_periods: usize,
    /// Upper bound for one provider call. Outputs are assembled in stable input order.
    pub max_batch_items: usize,
    pub cpu_units: u32,
    pub memory_budget_bytes: u64,
    pub max_job_memory_bytes: u64,
    pub nested_parallelism: NestedParallelism,
}

impl Default for EngineConfig {
    fn default() -> Self {
        Self {
            worker_count: 1,
            queue_capacity: 32,
            max_products: 100_000,
            max_payment_periods: 512,
            max_batch_items: 1024,
            cpu_units: 1,
            memory_budget_bytes: 1 << 30,
            max_job_memory_bytes: 256 << 20,
            nested_parallelism: NestedParallelism::Disabled,
        }
    }
}

impl EngineConfig {
    /// Read deployment guardrails without coupling the domain/API crates to a config library.
    /// Missing variables retain `Default`; malformed values fail startup rather than silently
    /// changing concurrency.
    pub fn from_env() -> Result<Self, QuantError> {
        let mut config = Self::default();
        config.worker_count = env_usize("QUANT_WORKER_COUNT", config.worker_count)?;
        config.queue_capacity = env_usize("QUANT_QUEUE_CAPACITY", config.queue_capacity)?;
        config.cpu_units = env_u32("QUANT_CPU_UNITS", config.cpu_units)?;
        config.memory_budget_bytes =
            env_u64("QUANT_MEMORY_BUDGET_BYTES", config.memory_budget_bytes)?;
        config.max_job_memory_bytes =
            env_u64("QUANT_MAX_JOB_MEMORY_BYTES", config.max_job_memory_bytes)?;
        config.nested_parallelism = match std::env::var("QUANT_NESTED_PARALLELISM").ok().as_deref()
        {
            None | Some("disabled") => NestedParallelism::Disabled,
            Some("enabled") => NestedParallelism::Enabled,
            Some(value) => {
                return Err(QuantError::InvalidRequest(format!(
                    "QUANT_NESTED_PARALLELISM must be disabled or enabled, got {value}"
                )))
            }
        };
        Ok(config)
    }
}

fn env_usize(name: &str, default: usize) -> Result<usize, QuantError> {
    match std::env::var(name) {
        Ok(value) => value
            .parse()
            .map_err(|_| QuantError::InvalidRequest(format!("{name} must be a positive integer"))),
        Err(std::env::VarError::NotPresent) => Ok(default),
        Err(error) => Err(QuantError::InvalidRequest(format!(
            "cannot read {name}: {error}"
        ))),
    }
}

fn env_u32(name: &str, default: u32) -> Result<u32, QuantError> {
    match std::env::var(name) {
        Ok(value) => value
            .parse()
            .map_err(|_| QuantError::InvalidRequest(format!("{name} must be a positive integer"))),
        Err(std::env::VarError::NotPresent) => Ok(default),
        Err(error) => Err(QuantError::InvalidRequest(format!(
            "cannot read {name}: {error}"
        ))),
    }
}

fn env_u64(name: &str, default: u64) -> Result<u64, QuantError> {
    match std::env::var(name) {
        Ok(value) => value
            .parse()
            .map_err(|_| QuantError::InvalidRequest(format!("{name} must be a positive integer"))),
        Err(std::env::VarError::NotPresent) => Ok(default),
        Err(error) => Err(QuantError::InvalidRequest(format!(
            "cannot read {name}: {error}"
        ))),
    }
}

/// Cooperative cancellation checked at every bounded provider batch. A provider call already in
/// flight is allowed to finish; no new chunk starts after cancellation is observed.
#[derive(Debug, Clone, Default)]
pub struct ExecutionControl {
    cancelled: Arc<AtomicBool>,
    processed_chunks: Arc<AtomicUsize>,
    total_chunks: Arc<AtomicUsize>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProgressSnapshot {
    pub completed_chunks: usize,
    pub total_chunks: usize,
    pub cancelled: bool,
}

impl ExecutionControl {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }
    pub fn cancellation_token(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.cancelled)
    }
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
    pub fn processed_chunks(&self) -> usize {
        self.processed_chunks.load(Ordering::Acquire)
    }
    pub fn set_total_chunks(&self, total: usize) {
        self.total_chunks.store(total, Ordering::Release);
    }
    pub fn total_chunks(&self) -> usize {
        self.total_chunks.load(Ordering::Acquire)
    }
    pub fn progress(&self) -> (usize, usize) {
        (self.processed_chunks(), self.total_chunks())
    }
    pub fn progress_snapshot(&self) -> ProgressSnapshot {
        ProgressSnapshot {
            completed_chunks: self.processed_chunks(),
            total_chunks: self.total_chunks(),
            cancelled: self.is_cancelled(),
        }
    }
    fn check(&self) -> Result<(), QuantError> {
        if self.is_cancelled() {
            Err(QuantError::Cancelled)
        } else {
            Ok(())
        }
    }
    fn mark_chunk(&self) {
        self.processed_chunks.fetch_add(1, Ordering::AcqRel);
    }
}

#[derive(Debug, Error, Clone)]
pub enum QuantError {
    #[error("invalid request: {0}")]
    InvalidRequest(String),
    #[error("resource not found: {kind}/{id}")]
    ResourceNotFound { kind: String, id: String },
    #[error("unsupported measure: {0}")]
    UnsupportedMeasure(String),
    #[error("pricing queue is full")]
    QueueFull,
    #[error("pricing request was cancelled before execution")]
    Cancelled,
    #[error("pricing worker stopped")]
    WorkerStopped,
    #[error("domain error: {0}")]
    Domain(String),
    #[error("legacy backend {provider}: {code}: {message}")]
    Legacy {
        provider: String,
        code: u32,
        message: String,
    },
}

impl From<DomainError> for QuantError {
    fn from(value: DomainError) -> Self {
        Self::Domain(value.to_string())
    }
}

struct Job {
    execute: Box<dyn FnOnce() + Send + 'static>,
    _admission: AdmissionPermit,
}

#[derive(Clone)]
pub struct Engine {
    config: EngineConfig,
    tx: mpsc::SyncSender<Job>,
    queued: Arc<AtomicUsize>,
    registry: Arc<KernelRegistry>,
    admission: Arc<WeightedAdmission>,
}

pub type PricingService = Engine;

#[derive(Debug)]
pub struct JobHandle {
    cancelled: Arc<AtomicBool>,
    result: mpsc::Receiver<Result<PricingOutput, QuantError>>,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct TradePrice {
    pub trade_id: String,
    pub value: f64,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct PortfolioPricingResult {
    pub values: Vec<f64>,
    pub trade_ids: Vec<String>,
    pub trades: Vec<TradePrice>,
    pub failures: Vec<PartialFailure>,
    pub chunks: usize,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct ScenarioPricingRow {
    pub scenario_id: String,
    pub values: Vec<f64>,
    pub trade_ids: Vec<String>,
    pub failures: Vec<PartialFailure>,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct ScenarioResult {
    pub scenarios: Vec<ScenarioPricingRow>,
    pub failures: Vec<PartialFailure>,
    pub chunks: usize,
    /// Kernels in this phase are not checkpoint/restart capable.
    pub continuation_token: Option<String>,
}

type PortfolioMember = (usize, Trade, ProductHandle);
type PortfolioGroup = (String, Vec<PortfolioMember>);

impl JobHandle {
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }
    pub fn wait(self) -> Result<PricingOutput, QuantError> {
        self.result.recv().unwrap_or(Err(QuantError::WorkerStopped))
    }
}

impl Engine {
    pub fn new(config: EngineConfig) -> Result<Self, QuantError> {
        let mut config = config;
        if config.worker_count == 0 || config.queue_capacity == 0 {
            return Err(QuantError::InvalidRequest(
                "worker_count and queue_capacity must be positive".into(),
            ));
        }
        if config.memory_budget_bytes == 0 || config.max_job_memory_bytes == 0 {
            return Err(QuantError::InvalidRequest(
                "memory budgets must be positive".into(),
            ));
        }
        if config.max_job_memory_bytes > config.memory_budget_bytes {
            return Err(QuantError::InvalidRequest(
                "max_job_memory_bytes cannot exceed memory_budget_bytes".into(),
            ));
        }
        let cpu_units = if config.cpu_units == 0 {
            config.worker_count as u32
        } else {
            config.cpu_units.max(config.worker_count as u32)
        };
        config.cpu_units = cpu_units;
        let admission =
            WeightedAdmission::new(AdmissionLimits::new(cpu_units, config.memory_budget_bytes));
        let (tx, rx): (mpsc::SyncSender<Job>, mpsc::Receiver<Job>) =
            mpsc::sync_channel(config.queue_capacity);
        let receiver = Arc::new(Mutex::new(rx));
        let queued = Arc::new(AtomicUsize::new(0));
        for index in 0..config.worker_count {
            let receiver = Arc::clone(&receiver);
            let queued = Arc::clone(&queued);
            thread::Builder::new()
                .name(format!("quant-compute-{index}"))
                .spawn(move || loop {
                    let job = match receiver.lock().expect("scheduler receiver lock").recv() {
                        Ok(job) => job,
                        Err(_) => break,
                    };
                    queued.fetch_sub(1, Ordering::AcqRel);
                    (job.execute)();
                })
                .map_err(|error| {
                    QuantError::InvalidRequest(format!("cannot start worker: {error}"))
                })?;
        }
        let mut registry = KernelRegistry::new();
        registry.register(Arc::new(RustIrsKernel::new()));
        Ok(Self {
            config,
            tx,
            queued,
            registry: Arc::new(registry),
            admission,
        })
    }

    pub fn config(&self) -> &EngineConfig {
        &self.config
    }
    pub fn queue_depth(&self) -> usize {
        self.queued.load(Ordering::Acquire)
    }
    pub fn is_ready(&self) -> bool {
        self.queue_depth() < self.config.queue_capacity
            && self.admission.snapshot().cpu_in_use < self.admission.limits().cpu_units
    }

    pub fn admission(&self) -> AdmissionSnapshot {
        self.admission.snapshot()
    }

    /// Stop accepting work and let fixed workers exit after their current job.
    pub fn shutdown(self) {
        drop(self);
    }

    pub fn price(&self, input: PricingInput) -> Result<PricingOutput, QuantError> {
        self.submit_price(input)?.wait()
    }

    pub fn submit_price(&self, input: PricingInput) -> Result<JobHandle, QuantError> {
        self.submit_price_with_cancel(input, Arc::new(AtomicBool::new(false)))
    }

    pub fn submit_price_with_cancel(
        &self,
        input: PricingInput,
        cancelled: Arc<AtomicBool>,
    ) -> Result<JobHandle, QuantError> {
        validate_request(&self.config, &input)?;
        if cancelled.load(Ordering::Acquire) {
            return Err(QuantError::Cancelled);
        }
        let cost = estimate_job_cost(&self.config, &input);
        if cost.estimated_bytes > self.config.max_job_memory_bytes {
            return Err(QuantError::InvalidRequest(
                "estimated job memory exceeds max_job_memory_bytes".into(),
            ));
        }
        let admission = self
            .admission
            .try_acquire(cost)
            .ok_or(QuantError::QueueFull)?;
        let (result, receiver) = mpsc::channel();
        let worker_config = self.config.clone();
        let cancelled_for_handle = Arc::clone(&cancelled);
        self.enqueue(admission, move || {
            if cancelled.load(Ordering::Acquire) {
                let _ = result.send(Err(QuantError::Cancelled));
            } else {
                let _ = result.send(price_owned(&worker_config, &input));
            }
        })?;
        Ok(JobHandle {
            cancelled: cancelled_for_handle,
            result: receiver,
        })
    }

    /// Async adapters can publish the result directly to a oneshot without nesting a submission
    /// while a fixed worker is already occupied. The callback executes on the CPU pool thread.
    pub fn submit_price_callback<F>(
        &self,
        input: PricingInput,
        cancelled: Arc<AtomicBool>,
        callback: F,
    ) -> Result<(), QuantError>
    where
        F: FnOnce(Result<PricingOutput, QuantError>) + Send + 'static,
    {
        validate_request(&self.config, &input)?;
        if cancelled.load(Ordering::Acquire) {
            return Err(QuantError::Cancelled);
        }
        let cost = estimate_job_cost(&self.config, &input);
        if cost.estimated_bytes > self.config.max_job_memory_bytes {
            return Err(QuantError::InvalidRequest(
                "estimated job memory exceeds max_job_memory_bytes".into(),
            ));
        }
        let admission = self
            .admission
            .try_acquire(cost)
            .ok_or(QuantError::QueueFull)?;
        let worker_config = self.config.clone();
        self.enqueue(admission, move || {
            if cancelled.load(Ordering::Acquire) {
                callback(Err(QuantError::Cancelled));
            } else {
                callback(price_owned(&worker_config, &input));
            }
        })
    }

    /// Submit an arbitrary CPU task to the same fixed worker pool used by legacy pricing. The
    /// closure runs outside Tokio; it is responsible for publishing its result (usually through
    /// a `tokio::sync::oneshot::Sender` in the API adapter).
    pub fn submit_task<F>(
        &self,
        cost: JobCost,
        cancelled: Arc<AtomicBool>,
        task: F,
    ) -> Result<(), QuantError>
    where
        F: FnOnce() + Send + 'static,
    {
        if cancelled.load(Ordering::Acquire) {
            return Err(QuantError::Cancelled);
        }
        if cost.estimated_bytes > self.config.max_job_memory_bytes {
            return Err(QuantError::InvalidRequest(
                "estimated job memory exceeds max_job_memory_bytes".into(),
            ));
        }
        let admission = self
            .admission
            .try_acquire(cost)
            .ok_or(QuantError::QueueFull)?;
        self.enqueue(admission, task)
    }

    fn enqueue<F>(&self, admission: AdmissionPermit, task: F) -> Result<(), QuantError>
    where
        F: FnOnce() + Send + 'static,
    {
        self.queued.fetch_add(1, Ordering::AcqRel);
        let job = Job {
            execute: Box::new(task),
            _admission: admission,
        };
        if let Err(mpsc::TrySendError::Full(_)) = self.tx.try_send(job) {
            self.queued.fetch_sub(1, Ordering::AcqRel);
            return Err(QuantError::QueueFull);
        }
        Ok(())
    }

    pub fn apply_context(
        &self,
        context: &QuantContext,
        commands: &[ContextCommand],
    ) -> Result<(QuantContext, Option<ResourceRef>), QuantError> {
        Ok(context.apply(commands)?)
    }

    /// Construct an engine with the phase-2 provider registry. The legacy REST scheduler
    /// remains available; typed requests use `compile_plan`/`price_typed` below.
    pub fn with_providers(
        config: EngineConfig,
        providers: Vec<Arc<dyn PricingKernel>>,
    ) -> Result<Self, QuantError> {
        let mut engine = Self::new(config)?;
        let mut registry = KernelRegistry::new();
        for provider in providers {
            registry.register(provider);
        }
        engine.registry = Arc::new(registry);
        Ok(engine)
    }

    pub fn registry(&self) -> &KernelRegistry {
        &self.registry
    }

    pub fn compile_plan(&self, request: &PlanRequest) -> Result<ExecutionPlan, QuantError> {
        self.registry
            .compile_plan(request)
            .map_err(map_planner_error)
    }

    pub fn price_typed(&self, request: PlanRequest) -> Result<Vec<f64>, QuantError> {
        // A single-item call is intentionally just the batch path with one item.
        let plan = self
            .registry
            .compile_plan_bounded(&request, self.config.max_batch_items)
            .map_err(map_planner_error)?;
        self.registry.execute(&plan).map_err(map_planner_error)
    }

    /// Price heterogeneous trades in grouped, bounded provider calls. Product references and
    /// outputs remain in the caller's original order; a failed group is reported explicitly.
    pub fn price_portfolio(
        &self,
        portfolio: &Portfolio,
        model: &ModelHandle,
        market: &MarketHandle,
        context: &PricingContext,
    ) -> Result<PortfolioPricingResult, QuantError> {
        self.price_portfolio_with_control(
            portfolio,
            model,
            market,
            context,
            &ExecutionControl::new(),
        )
    }

    pub fn price_portfolio_with_control(
        &self,
        portfolio: &Portfolio,
        model: &ModelHandle,
        market: &MarketHandle,
        context: &PricingContext,
        control: &ExecutionControl,
    ) -> Result<PortfolioPricingResult, QuantError> {
        control.check()?;
        if portfolio.trades.len() > self.config.max_products {
            return Err(QuantError::InvalidRequest(
                "portfolio exceeds max_products".into(),
            ));
        }
        if control.total_chunks() == 0 {
            control.set_total_chunks(
                portfolio
                    .trades
                    .len()
                    .div_ceil(self.config.max_batch_items.max(1)),
            );
        }
        let mut values = vec![None; portfolio.trades.len()];
        let mut failures = Vec::<(usize, PartialFailure)>::new();
        let mut groups: Vec<PortfolioGroup> = Vec::new();
        for (index, trade) in portfolio.trades.iter().cloned().enumerate() {
            let product = match context_product_handle(&trade) {
                Ok(product) => product,
                Err(error) => {
                    failures.push((
                        index,
                        PartialFailure {
                            item_id: trade.trade_id.0,
                            error,
                        },
                    ));
                    continue;
                }
            };
            let family = compatibility_layout_key(&product);
            if let Some((_, members)) = groups.iter_mut().find(|(kind, _)| *kind == family) {
                members.push((index, trade, product));
            } else {
                groups.push((family, vec![(index, trade, product)]));
            }
        }
        let mut chunks = 0;
        for (_, members) in groups {
            control.check()?;
            let handles = members
                .iter()
                .map(|(_, _, product)| product.clone())
                .collect::<Vec<_>>();
            let request = plan_request(market.clone(), model.clone(), handles, context);
            let plan = match self
                .registry
                .compile_plan_bounded(&request, self.config.max_batch_items)
            {
                Ok(plan) => plan,
                Err(error) => {
                    for (index, trade, _) in members {
                        failures.push((
                            index,
                            PartialFailure {
                                item_id: trade.trade_id.0,
                                error: error.to_string(),
                            },
                        ));
                    }
                    continue;
                }
            };
            let mut cursor = 0;
            for batch in &plan.batches {
                control.check()?;
                let output = match self.registry.execute(&ExecutionPlan {
                    batches: vec![batch.clone()],
                    fingerprint: plan.fingerprint.clone(),
                }) {
                    Ok(output) => output,
                    Err(error) => {
                        for (index, trade, _) in &members[cursor..cursor + batch.items.len()] {
                            failures.push((
                                *index,
                                PartialFailure {
                                    item_id: trade.trade_id.0.clone(),
                                    error: error.to_string(),
                                },
                            ));
                        }
                        cursor += batch.items.len();
                        continue;
                    }
                };
                control.mark_chunk();
                chunks += 1;
                for value in output {
                    let (index, trade, _) = &members[cursor];
                    values[*index] = Some(value * trade.quantity);
                    cursor += 1;
                }
            }
        }
        let mut trade_ids = Vec::new();
        let mut trade_values = Vec::new();
        let mut trades = Vec::new();
        for (index, trade) in portfolio.trades.iter().enumerate() {
            if let Some(value) = values[index] {
                trade_ids.push(trade.trade_id.0.clone());
                trade_values.push(value);
                trades.push(TradePrice {
                    trade_id: trade.trade_id.0.clone(),
                    value,
                });
            }
        }
        failures.sort_by_key(|(index, _)| *index);
        Ok(PortfolioPricingResult {
            values: trade_values,
            trade_ids,
            trades,
            failures: failures.into_iter().map(|(_, failure)| failure).collect(),
            chunks,
        })
    }

    /// Calculate EE/PFE/CVA/DVA/FVA/MVA/KVA through one shared exposure reduction graph.
    /// When no cube is supplied, a bounded one-path cube is derived from the portfolio PVs;
    /// callers that own a Monte Carlo provider should pass Q/P cube artifacts explicitly.
    pub fn calculate_xva(&self, request: &XvaRequest) -> Result<XvaResult, QuantError> {
        self.calculate_xva_with_control(request, &ExecutionControl::new())
    }

    pub fn calculate_xva_with_control(
        &self,
        request: &XvaRequest,
        control: &ExecutionControl,
    ) -> Result<XvaResult, QuantError> {
        control.check()?;
        if request.operation_id.trim().is_empty() {
            return Err(QuantError::InvalidRequest(
                "operation_id is required".into(),
            ));
        }
        if request.context.context_hash.is_some() && !request.context.verify_hash() {
            return Err(QuantError::InvalidRequest(
                "context_hash does not match context".into(),
            ));
        }
        let q_cube = if let Some(cube) = request.q_exposure.clone() {
            cube
        } else {
            let portfolio = request
                .context
                .portfolio(&request.portfolio_id)
                .map_err(|e| QuantError::Domain(e.to_string()))?;
            let market = request
                .context
                .market_handle(&request.market.id)
                .map_err(|e| QuantError::Domain(e.to_string()))?;
            let model = request
                .context
                .model_handle(&request.model.id)
                .map_err(|e| QuantError::Domain(e.to_string()))?;
            let priced = self.price_portfolio_with_control(
                &portfolio,
                &model,
                &market,
                &request.pricing,
                control,
            )?;
            // Netting is applied before exposure reductions.  Each declared/default set is
            // reduced independently, then the resulting XVAs are added; this avoids the
            // incorrect shortcut of netting unrelated counterparties together.
            let mut grouped = BTreeMap::<String, f64>::new();
            for trade in &portfolio.trades {
                if let Some(value) = priced
                    .trades
                    .iter()
                    .find(|row| row.trade_id == trade.trade_id.0)
                    .map(|row| row.value)
                {
                    let key = trade
                        .netting_set_id
                        .as_ref()
                        .map(|id| id.0.clone())
                        .unwrap_or_else(|| "__default__".into());
                    *grouped.entry(key).or_default() += value;
                }
            }
            let total = grouped.values().copied().sum::<f64>();
            let n_steps = request.pricing.n_steps.max(1) as usize;
            let n_paths = request.pricing.n_paths.clamp(1, 1_000) as usize;
            let times = (0..n_steps)
                .map(|i| (i + 1) as f64 / n_steps as f64)
                .collect::<Vec<_>>();
            if grouped.len() <= 1 || request.p_exposure.is_some() {
                ExposureCubeInput {
                    measure: quant_domain::ProbabilityMeasure::Q,
                    times,
                    paths: vec![vec![total; n_paths]; n_steps],
                    discount_factors: Vec::new(),
                    source_hash: request.context.context_hash.clone(),
                }
            } else {
                // Preserve independent netting sets as deterministic path columns. The
                // grouped reduction below converts these columns into a sum of set-level
                // metrics rather than averaging counterparties as if they were scenarios.
                let mut values = grouped.into_values().collect::<Vec<_>>();
                values.sort_by(f64::total_cmp);
                let mut result: Option<XvaResult> = None;
                for value in values {
                    let cube = ExposureCubeInput {
                        measure: quant_domain::ProbabilityMeasure::Q,
                        times: times.clone(),
                        paths: vec![vec![value; n_paths]; n_steps],
                        discount_factors: Vec::new(),
                        source_hash: request.context.context_hash.clone(),
                    };
                    let partial = xva::calculate(request, &cube, request.p_exposure.as_ref())
                        .map_err(QuantError::InvalidRequest)?;
                    if let Some(aggregate) = result.as_mut() {
                        for (dst, src) in aggregate.ee.iter_mut().zip(partial.ee.iter()) {
                            *dst += src;
                        }
                        for (dst, src) in aggregate.pfe.iter_mut().zip(partial.pfe.iter()) {
                            *dst += src;
                        }
                        aggregate.cva += partial.cva;
                        aggregate.dva += partial.dva;
                        aggregate.fva += partial.fva;
                        aggregate.mva += partial.mva;
                        aggregate.kva += partial.kva;
                        for (key, value) in &partial.measures {
                            *aggregate.measures.entry(key.clone()).or_default() += value;
                        }
                        aggregate.artifacts.extend(partial.artifacts);
                    } else {
                        result = Some(partial);
                    }
                }
                let mut aggregate = result.ok_or_else(|| {
                    QuantError::InvalidRequest("portfolio has no priced trades".into())
                })?;
                aggregate
                    .provenance
                    .approximations
                    .push("netting sets reduced independently before XVA aggregation".into());
                return Ok(aggregate);
            }
        };
        if let Some(budget) = request.memory_budget_bytes {
            if xva::estimate_memory_bytes(&q_cube) > budget {
                return Err(QuantError::InvalidRequest("XVA exposure exceeds memory_budget_bytes; use streaming chunks or a smaller cube".into()));
            }
        }
        let p_cube = request.p_exposure.as_ref();
        xva::calculate(request, &q_cube, p_cube).map_err(QuantError::InvalidRequest)
    }

    /// Evaluate a shared scenario set in bounded scenario chunks. Each scenario reuses the same
    /// immutable portfolio/model and plan cache; no continuation token is advertised because the
    /// current kernels are not resumable.
    pub fn run_scenarios(
        &self,
        portfolio: &Portfolio,
        scenarios: &ScenarioSet,
        model: &ModelHandle,
        market: &MarketHandle,
        context: &PricingContext,
    ) -> Result<ScenarioResult, QuantError> {
        self.run_scenarios_with_control(
            portfolio,
            scenarios,
            model,
            market,
            context,
            &ExecutionControl::new(),
        )
    }

    pub fn run_scenarios_with_control(
        &self,
        portfolio: &Portfolio,
        scenarios: &ScenarioSet,
        model: &ModelHandle,
        market: &MarketHandle,
        context: &PricingContext,
        control: &ExecutionControl,
    ) -> Result<ScenarioResult, QuantError> {
        control.check()?;
        let chunk_size = scenarios
            .chunk_size
            .unwrap_or(self.config.max_batch_items)
            .max(1);
        control.set_total_chunks(
            scenarios.scenarios.len().saturating_mul(
                portfolio
                    .trades
                    .len()
                    .div_ceil(self.config.max_batch_items.max(1)),
            ),
        );
        let mut rows = Vec::with_capacity(scenarios.scenarios.len());
        let mut failures = Vec::new();
        let mut chunks = 0;
        for scenario_chunk in scenarios.scenarios.chunks(chunk_size) {
            for scenario in scenario_chunk {
                control.check()?;
                let scenario_market = scenario_market(market, scenario);
                let priced = self.price_portfolio_with_control(
                    portfolio,
                    model,
                    &scenario_market,
                    context,
                    control,
                )?;
                chunks += priced.chunks.max(1);
                failures.extend(priced.failures.iter().cloned().map(|mut failure| {
                    failure.item_id = format!("{}:{}", scenario.id, failure.item_id);
                    failure
                }));
                rows.push(ScenarioPricingRow {
                    scenario_id: scenario.id.0.clone(),
                    values: priced.values,
                    trade_ids: priced.trade_ids,
                    failures: priced.failures,
                });
            }
        }
        Ok(ScenarioResult {
            scenarios: rows,
            failures,
            chunks,
            continuation_token: None,
        })
    }
}

fn context_product_handle(trade: &Trade) -> Result<ProductHandle, String> {
    let spec = trade.product.as_ref().ok_or_else(|| {
        format!(
            "product {} is not embedded in the portfolio",
            trade.product_id
        )
    })?;
    Ok(ProductHandle::new(quant_domain::ProductEntry {
        id: trade.product_id.clone(),
        hash: spec.hash(),
        spec: spec.clone(),
        version: 0,
        parent_hash: None,
    }))
}

fn plan_request(
    market: MarketHandle,
    model: ModelHandle,
    products: Vec<ProductHandle>,
    context: &PricingContext,
) -> PlanRequest {
    let context_hash = hash_value(&serde_json::json!({
        "market": market.hash(), "model": model.hash(),
        "products": products.iter().map(|product| product.hash()).collect::<Vec<_>>(),
        "context": context,
    }));
    PlanRequest {
        market,
        model,
        products,
        measure: context.measure.clone().unwrap_or_else(|| "PV".into()),
        context: context.clone(),
        context_hash,
    }
}

fn scenario_market(market: &MarketHandle, scenario: &quant_domain::Scenario) -> MarketHandle {
    let mut spec = market.snapshot().spec.clone();
    for key in ["r0", "market.r0", "discount_rate"] {
        if let Some(shock) = scenario.shocks.get(key) {
            let base = spec.params.get(key).and_then(Value::as_f64).unwrap_or(0.0);
            spec.params
                .insert(key.to_owned(), Value::from(base + shock));
        }
    }
    MarketHandle::new(MarketSnapshot::new(
        market.id().clone(),
        spec,
        market.version(),
        Some(market.hash().clone()),
    ))
}

fn map_planner_error(error: PlannerError) -> QuantError {
    match error {
        PlannerError::Legacy {
            provider,
            code,
            message,
        } => QuantError::Legacy {
            provider,
            code,
            message,
        },
        other => QuantError::InvalidRequest(other.to_string()),
    }
}

fn validate_request(config: &EngineConfig, input: &PricingInput) -> Result<(), QuantError> {
    if input.operation_id.trim().is_empty() {
        return Err(QuantError::InvalidRequest(
            "operation_id is required".into(),
        ));
    }
    if input.context.schema != quant_domain::CONTEXT_SCHEMA {
        return Err(QuantError::InvalidRequest(
            "unsupported context schema".into(),
        ));
    }
    if input.context.context_hash.is_some() && !input.context.verify_hash() {
        return Err(QuantError::InvalidRequest(
            "context_hash does not match context".into(),
        ));
    }
    if input.products.len() > config.max_products {
        return Err(QuantError::InvalidRequest("product limit exceeded".into()));
    }
    if input.measures.is_empty() {
        return Err(QuantError::InvalidRequest(
            "at least one measure is required".into(),
        ));
    }
    for measure in &input.measures {
        if !measure.name.eq_ignore_ascii_case("pv")
            && !measure.name.eq_ignore_ascii_case("present_value")
        {
            return Err(QuantError::UnsupportedMeasure(measure.name.clone()));
        }
    }
    Ok(())
}

fn estimate_job_cost(config: &EngineConfig, input: &PricingInput) -> JobCost {
    // Inputs are JSON values, so sizing them by serialized bytes is deterministic and avoids
    // pretending to know allocator overhead. Keep a floor for tiny analytical requests.
    let bytes = serde_json::to_vec(input)
        .map(|value| value.len() as u64)
        .unwrap_or(config.max_job_memory_bytes);
    let estimated_bytes = bytes
        .saturating_mul(2)
        .max(4096)
        .min(config.max_job_memory_bytes);
    JobCost::new(1, estimated_bytes)
}

fn price_owned(config: &EngineConfig, input: &PricingInput) -> Result<PricingOutput, QuantError> {
    let market = lookup_resource(&input.context.markets, input.market.as_ref(), "market")?;
    let model = lookup_resource(&input.context.models, input.model.as_ref(), "model")?;
    let product = if let Some(reference) = input.products.first() {
        lookup_resource(&input.context.products, Some(reference), "product")?
    } else {
        input
            .context
            .products
            .values()
            .next()
            .ok_or_else(|| QuantError::ResourceNotFound {
                kind: "product".into(),
                id: "<first>".into(),
            })?
    };
    let model = unwrap_params(model);
    let market = unwrap_params(market);
    let product = unwrap_params(product);
    let payment_times = value_vec_f64(product, "payment_times")?;
    let accruals = value_vec_f64(product, "accruals")?;
    if payment_times.is_empty() || payment_times.len() != accruals.len() {
        return Err(QuantError::InvalidRequest(
            "payment_times and accruals must have the same non-zero length".into(),
        ));
    }
    if payment_times.len() > config.max_payment_periods {
        return Err(QuantError::InvalidRequest(
            "payment period limit exceeded".into(),
        ));
    }
    if payment_times
        .iter()
        .zip(accruals.iter())
        .any(|(time, accrual)| {
            !time.is_finite() || !accrual.is_finite() || *time <= 0.0 || *accrual <= 0.0
        })
        || payment_times
            .windows(2)
            .any(|window| window[1] <= window[0])
    {
        return Err(QuantError::InvalidRequest(
            "payment_times must be finite, positive and strictly increasing; accruals must be positive".into(),
        ));
    }
    let a = value_f64(model, "a", 0.1)?;
    let b = value_f64(model, "b", 0.03)?;
    let sigma = value_f64(model, "sigma", 0.01)?;
    let r0 = value_f64(market, "r0", value_f64(model, "r0", 0.02)?)?;
    let notional = value_f64(product, "notional", 1_000_000.0)?;
    let fixed_rate = value_f64(product, "fixed_rate", 0.0)?;
    let use_par_rate = value_bool(product, "use_par_rate", false)?;
    let start = value_f64(product, "start", 0.0)?;
    let pv = engine_core::api::irs_hull_white_npv(
        a,
        b,
        sigma,
        r0,
        notional,
        fixed_rate,
        use_par_rate,
        start,
        payment_times,
        accruals,
    );
    let mut result = BTreeMap::new();
    result.insert("pv".into(), json!(pv));
    let mut provenance = BTreeMap::new();
    provenance.insert("backend".into(), json!("cpu"));
    provenance.insert("engine".into(), json!("engine-core"));
    provenance.insert("operation_id".into(), json!(input.operation_id));
    Ok(PricingOutput {
        context: input.context.clone(),
        result,
        provenance,
    })
}

fn lookup_resource<'a>(
    resources: &'a BTreeMap<String, Value>,
    reference: Option<&ResourceRef>,
    kind: &str,
) -> Result<&'a Value, QuantError> {
    match reference {
        Some(reference) => {
            resources
                .get(&reference.id)
                .ok_or_else(|| QuantError::ResourceNotFound {
                    kind: kind.into(),
                    id: reference.id.clone(),
                })
        }
        None => resources
            .values()
            .next()
            .ok_or_else(|| QuantError::ResourceNotFound {
                kind: kind.into(),
                id: "<first>".into(),
            }),
    }
}

fn unwrap_params(value: &Value) -> &Value {
    value.get("params").unwrap_or(value)
}

fn value_f64(value: &Value, key: &str, default: f64) -> Result<f64, QuantError> {
    match value.get(key) {
        None => Ok(default),
        Some(Value::Number(number)) => number
            .as_f64()
            .ok_or_else(|| QuantError::InvalidRequest(format!("{key} must be finite"))),
        Some(other) => Err(QuantError::InvalidRequest(format!(
            "{key} must be a number, got {other}"
        ))),
    }
}

fn value_bool(value: &Value, key: &str, default: bool) -> Result<bool, QuantError> {
    match value.get(key) {
        None => Ok(default),
        Some(Value::Bool(value)) => Ok(*value),
        Some(_) => Err(QuantError::InvalidRequest(format!(
            "{key} must be a boolean"
        ))),
    }
}

fn value_vec_f64(value: &Value, key: &str) -> Result<Vec<f64>, QuantError> {
    let values = value
        .get(key)
        .ok_or_else(|| QuantError::InvalidRequest(format!("{key} is required")))?
        .as_array()
        .ok_or_else(|| QuantError::InvalidRequest(format!("{key} must be an array")))?;
    values
        .iter()
        .map(|v| {
            v.as_f64()
                .ok_or_else(|| QuantError::InvalidRequest(format!("{key} contains a non-number")))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use quant_domain::{ContextCommand, MeasureSpec};

    fn request() -> PricingInput {
        let context = QuantContext::new("test").apply(&[
            ContextCommand::AddMarket { id: "m".into(), spec: json!({"r0":0.02}) },
            ContextCommand::AddModel { id: "hw".into(), spec: json!({"a":0.1,"b":0.03,"sigma":0.01}) },
            ContextCommand::AddProduct { id: "irs".into(), spec: json!({"notional":1_000_000.0,"fixed_rate":0.02,"payment_times":[1.0,2.0],"accruals":[1.0,1.0]}) },
        ]).unwrap().0;
        PricingInput {
            context,
            operation_id: "op-1".into(),
            market: Some(ResourceRef {
                id: "m".into(),
                hash: "".into(),
                kind: "market".into(),
                version: 1,
            }),
            model: Some(ResourceRef {
                id: "hw".into(),
                hash: "".into(),
                kind: "model".into(),
                version: 1,
            }),
            products: vec![ResourceRef {
                id: "irs".into(),
                hash: "".into(),
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

    #[test]
    fn scheduler_prices_on_bounded_worker() {
        let engine = Engine::new(EngineConfig {
            worker_count: 1,
            queue_capacity: 1,
            ..Default::default()
        })
        .unwrap();
        let result = engine.price(request()).unwrap();
        assert!(result.result["pv"].as_f64().is_some());
    }

    #[test]
    fn cancellation_before_execution_is_observed() {
        let engine = Engine::new(EngineConfig {
            worker_count: 1,
            queue_capacity: 1,
            ..Default::default()
        })
        .unwrap();
        let cancelled = Arc::new(AtomicBool::new(true));
        let err = engine
            .submit_price_with_cancel(request(), cancelled)
            .unwrap_err();
        assert!(matches!(err, QuantError::Cancelled));
    }

    #[test]
    fn shutdown_closes_fixed_workers_after_queued_task() {
        let engine = Engine::new(EngineConfig {
            worker_count: 1,
            queue_capacity: 1,
            ..Default::default()
        })
        .unwrap();
        let done = Arc::new(AtomicBool::new(false));
        let done_in_task = Arc::clone(&done);
        engine
            .submit_task(
                JobCost::new(1, 4096),
                Arc::new(AtomicBool::new(false)),
                move || done_in_task.store(true, Ordering::Release),
            )
            .unwrap();
        engine.shutdown();
        for _ in 0..100 {
            if done.load(Ordering::Acquire) {
                return;
            }
            thread::sleep(std::time::Duration::from_millis(1));
        }
        assert!(done.load(Ordering::Acquire));
    }

    #[test]
    fn the_same_client_owned_context_replays_on_two_engine_instances() {
        let first = Engine::new(EngineConfig::default()).unwrap();
        let second = Engine::new(EngineConfig::default()).unwrap();
        let input = request();
        let left = first.price(input.clone()).unwrap();
        let right = second.price(input).unwrap();
        assert_eq!(left.context.context_hash, right.context.context_hash);
        assert_eq!(left.result, right.result);
    }

    #[test]
    fn a_fresh_engine_after_restart_accepts_the_same_context() {
        let input = request();
        let before = Engine::new(EngineConfig::default())
            .unwrap()
            .price(input.clone())
            .unwrap();
        let after = Engine::new(EngineConfig::default())
            .unwrap()
            .price(input)
            .unwrap();
        assert_eq!(before.context.context_hash, after.context.context_hash);
        assert_eq!(before.result, after.result);
    }

    fn typed_engine() -> (Engine, PlanRequest) {
        let request = PlanRequest::inline(
            "m",
            quant_domain::MarketSpec::new(json!({"r0": 0.02})).unwrap(),
            "hw",
            quant_domain::ModelSpec::from_value(json!({"a": 0.1, "b": 0.03, "sigma": 0.01}))
                .unwrap(),
            "p",
            quant_domain::ProductSpec::from_value(json!({"payment_times":[1.0], "accruals":[1.0]}))
                .unwrap(),
            "PV",
            PricingContext::default(),
        );
        let descriptor = KernelDescriptor {
            id: KernelId::from("rust-test"),
            provider: ProviderId::from("rust"),
            model_kinds: vec!["hull_white_1f".into()],
            product_kinds: vec!["ir_swap".into()],
            measures: vec!["PV".into()],
            capabilities: Capabilities::empty(),
            input_layout_version: 1,
            priority: 0,
        };
        let engine = Engine::with_providers(
            EngineConfig {
                max_batch_items: 2,
                ..Default::default()
            },
            vec![Arc::new(ConstantKernel::new(descriptor, 7.0))],
        )
        .unwrap();
        (engine, request)
    }

    #[test]
    fn portfolio_batch_preserves_order_and_matches_single_values() {
        let (engine, request) = typed_engine();
        let spec = request.products[0].entry().spec.clone();
        let mut first = quant_domain::Trade::new("t-1", "p");
        first.product = Some(spec.clone());
        let mut second = quant_domain::Trade::new("t-2", "p");
        second.product = Some(spec);
        second.quantity = 2.0;
        let portfolio = quant_domain::Portfolio::new("book", vec![first, second]);
        let result = engine
            .price_portfolio(
                &portfolio,
                &request.model,
                &request.market,
                &request.context,
            )
            .unwrap();
        assert_eq!(result.trade_ids, vec!["t-1", "t-2"]);
        assert_eq!(result.values, vec![7.0, 14.0]);
        assert_eq!(result.failures, Vec::<quant_domain::PartialFailure>::new());
        assert_eq!(result.chunks, 1);
        // Eviction is safe: the context/product definitions remain authoritative.
        engine.registry().clear_cache();
        let replay = engine
            .price_portfolio(
                &portfolio,
                &request.model,
                &request.market,
                &request.context,
            )
            .unwrap();
        assert_eq!(replay.values, result.values);
    }

    #[test]
    fn scenario_set_is_shared_and_is_explicitly_not_resumable() {
        let (engine, request) = typed_engine();
        let mut trade = quant_domain::Trade::new("t", "p");
        trade.product = Some(request.products[0].entry().spec.clone());
        let portfolio = quant_domain::Portfolio::new("book", vec![trade]);
        let scenarios = quant_domain::ScenarioSet::new(
            "s",
            vec![
                quant_domain::Scenario::new("base", BTreeMap::new()),
                quant_domain::Scenario::new("up", BTreeMap::from([("r0".into(), 0.01)])),
            ],
        );
        let result = engine
            .run_scenarios(
                &portfolio,
                &scenarios,
                &request.model,
                &request.market,
                &request.context,
            )
            .unwrap();
        assert_eq!(result.scenarios.len(), 2);
        assert_eq!(result.scenarios[0].trade_ids, vec!["t"]);
        assert_eq!(result.continuation_token, None);
    }

    #[test]
    fn unsupported_group_is_a_partial_failure_not_a_silent_zero() {
        let (engine, request) = typed_engine();
        let mut good = quant_domain::Trade::new("good", "p");
        good.product = Some(request.products[0].entry().spec.clone());
        let mut bad = quant_domain::Trade::new("bad", "unsupported");
        bad.product =
            Some(quant_domain::ProductSpec::from_value(json!({"kind":"exotic"})).unwrap());
        let portfolio = quant_domain::Portfolio::new("book", vec![good, bad]);
        let result = engine
            .price_portfolio(
                &portfolio,
                &request.model,
                &request.market,
                &request.context,
            )
            .unwrap();
        assert_eq!(result.values, vec![7.0]);
        assert_eq!(result.failures.len(), 1);
        assert_eq!(result.failures[0].item_id, "bad");
    }

    #[test]
    fn cooperative_cancel_stops_before_the_next_bounded_chunk() {
        let (engine, request) = typed_engine();
        let mut trade = quant_domain::Trade::new("t", "p");
        trade.product = Some(request.products[0].entry().spec.clone());
        let portfolio = quant_domain::Portfolio::new("book", vec![trade; 10]);
        let control = ExecutionControl::new();
        control.cancel();
        let error = engine
            .price_portfolio_with_control(
                &portfolio,
                &request.model,
                &request.market,
                &request.context,
                &control,
            )
            .unwrap_err();
        assert!(matches!(error, QuantError::Cancelled));
        assert_eq!(control.processed_chunks(), 0);
    }
}
