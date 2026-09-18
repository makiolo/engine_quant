//! Optional GPU routing and resource control for API Phase 8.
//!
//! This module intentionally keeps Burn/WGPU types behind the `gpu` feature.  Products,
//! models, and the domain execution policy only carry a device preference; this layer owns
//! device discovery, precision checks, break-even routing, per-device queues, and memory
//! admission.  A CPU decision always carries a reason so an explicit GPU request can never
//! turn into a silent fallback.

use quant_domain::DevicePreference;
use std::collections::BTreeMap;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc, Mutex,
};
#[cfg(feature = "gpu")]
use std::time::Instant;
use thiserror::Error;

/// Policy used when an explicitly requested GPU cannot preserve the requested semantics.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum GpuFallbackPolicy {
    /// Return an error. This is the safe default for explicit GPU requests.
    #[default]
    Error,
    /// Route to CPU only when the CPU implementation preserves precision and semantics. The
    /// resulting [`GpuRouteDecision`] records that a fallback occurred.
    Cpu,
}

/// Numeric precision required by a workload. WGPU hardware commonly lacks native f64 support;
/// `F64` must therefore never be silently lowered to f32.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum GpuPrecision {
    #[default]
    F64,
    F32,
}

/// Stage timings used by routing and break-even harnesses. Values can be measured or explicitly
/// marked as proxy estimates; callers must not present proxy values as hardware measurements.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GpuTimings {
    pub cold_init_ns: u64,
    pub h2d_ns: u64,
    pub kernel_ns: u64,
    pub d2h_ns: u64,
}

impl GpuTimings {
    pub fn total_ns(self, resident: bool) -> u64 {
        self.cold_init_ns
            .saturating_add(self.kernel_ns)
            .saturating_add(if resident {
                0
            } else {
                self.h2d_ns.saturating_add(self.d2h_ns)
            })
    }
}

/// A batch description consumed by the router. `gpu_bytes` is the peak resident estimate, not
/// merely the input size, so the same value is charged to the per-device memory budget.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpuWorkload {
    pub batch_items: usize,
    pub gpu_bytes: u64,
    pub precision: GpuPrecision,
    pub resident: bool,
    pub cpu_ns: u64,
    pub gpu: GpuTimings,
}

impl Default for GpuWorkload {
    fn default() -> Self {
        Self {
            batch_items: 0,
            gpu_bytes: 0,
            precision: GpuPrecision::F64,
            resident: false,
            cpu_ns: 0,
            gpu: GpuTimings::default(),
        }
    }
}

/// Hardware-independent inventory entry. The runtime probe fills this structure; tests and
/// deployments can inject a deterministic inventory without requiring a physical GPU.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GpuDeviceInfo {
    pub id: u32,
    pub label: String,
    pub available: bool,
    pub supports_f64: bool,
    /// Zero means unknown (WGPU does not expose a portable VRAM total).
    pub memory_budget_bytes: u64,
}

impl GpuDeviceInfo {
    pub fn test_device(id: u32, supports_f64: bool, memory_budget_bytes: u64) -> Self {
        Self {
            id,
            label: format!("test-gpu-{id}"),
            available: true,
            supports_f64,
            memory_budget_bytes,
        }
    }
}

/// Inventory of devices available to the process.
#[derive(Debug, Clone, Default)]
pub struct GpuInventory {
    devices: BTreeMap<u32, GpuDeviceInfo>,
}

impl GpuInventory {
    pub fn new(devices: impl IntoIterator<Item = GpuDeviceInfo>) -> Self {
        Self {
            devices: devices
                .into_iter()
                .map(|device| (device.id, device))
                .collect(),
        }
    }

    pub fn devices(&self) -> impl Iterator<Item = &GpuDeviceInfo> {
        self.devices.values()
    }

    pub fn get(&self, id: u32) -> Option<&GpuDeviceInfo> {
        self.devices.get(&id)
    }

    pub fn available(&self) -> impl Iterator<Item = &GpuDeviceInfo> {
        self.devices.values().filter(|device| device.available)
    }

    /// Probe the default Burn WGPU adapter when compiled with `gpu`. CPU-only builds return an
    /// empty inventory and therefore keep `Auto` on CPU.
    pub fn detect() -> Self {
        let probe = probe_wgpu(None);
        if probe.available {
            Self::new([GpuDeviceInfo {
                id: 0,
                label: probe.label,
                available: true,
                supports_f64: probe.supports_f64,
                memory_budget_bytes: 0,
            }])
        } else {
            Self::default()
        }
    }
}

/// Explicit routing thresholds. Auto stays on CPU until both the batch threshold and measured
/// (or clearly labelled proxy) net-benefit threshold are met.
#[derive(Debug, Clone, Copy)]
pub struct GpuRoutingPolicy {
    pub min_batch_items: usize,
    pub min_net_speedup: f64,
    pub fallback: GpuFallbackPolicy,
}

impl Default for GpuRoutingPolicy {
    fn default() -> Self {
        Self {
            min_batch_items: 1_024,
            min_net_speedup: 0.05,
            fallback: GpuFallbackPolicy::Error,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuRouteTarget {
    Cpu,
    Gpu { device_id: u32 },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuRouteReason {
    ExplicitCpu,
    GpuSelected,
    AutoNoDevice,
    AutoPrecisionUnsupported,
    AutoMemoryBudget,
    AutoBelowThreshold,
    AutoNoNetBenefit,
    ExplicitUnavailable,
    ExplicitPrecisionUnsupported,
    ExplicitMemoryBudget,
    FallbackToCpu,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpuRouteDecision {
    pub target: GpuRouteTarget,
    pub reason: GpuRouteReason,
    pub fallback_used: bool,
}

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum GpuError {
    #[error("requested GPU device is unavailable")]
    DeviceUnavailable,
    #[error("requested GPU device does not support the required precision")]
    PrecisionUnsupported,
    #[error("GPU workload exceeds the device memory budget")]
    MemoryBudget,
    #[error("GPU support is not compiled in this build")]
    FeatureDisabled,
}

/// Routing layer shared by the engine and benchmark harness. It is deliberately independent of
/// a concrete Burn tensor so it can be exercised on CPU-only CI.
#[derive(Debug, Clone)]
pub struct GpuRouter {
    inventory: GpuInventory,
    policy: GpuRoutingPolicy,
}

impl GpuRouter {
    pub fn new(inventory: GpuInventory, policy: GpuRoutingPolicy) -> Self {
        Self { inventory, policy }
    }

    pub fn detect(policy: GpuRoutingPolicy) -> Self {
        Self::new(GpuInventory::detect(), policy)
    }

    pub fn inventory(&self) -> &GpuInventory {
        &self.inventory
    }

    pub fn policy(&self) -> GpuRoutingPolicy {
        self.policy
    }

    pub fn route(
        &self,
        preference: DevicePreference,
        workload: GpuWorkload,
    ) -> Result<GpuRouteDecision, GpuError> {
        match preference {
            DevicePreference::Cpu => Ok(GpuRouteDecision {
                target: GpuRouteTarget::Cpu,
                reason: GpuRouteReason::ExplicitCpu,
                fallback_used: false,
            }),
            DevicePreference::Auto => Ok(self.route_auto(workload)),
            DevicePreference::Gpu { device } => {
                // An explicitly named device is never replaced by another adapter. This keeps
                // placement and failure observable when a caller requested a particular GPU.
                let selected = match device {
                    Some(id) => self.inventory.get(id),
                    None => self.inventory.available().next(),
                };
                let failure = match selected {
                    None => Some(GpuError::DeviceUnavailable),
                    Some(info) if !info.available => Some(GpuError::DeviceUnavailable),
                    Some(info) if workload.precision == GpuPrecision::F64 && !info.supports_f64 => {
                        Some(GpuError::PrecisionUnsupported)
                    }
                    Some(info)
                        if info.memory_budget_bytes != 0
                            && workload.gpu_bytes > info.memory_budget_bytes =>
                    {
                        Some(GpuError::MemoryBudget)
                    }
                    Some(_) => None,
                };
                if let Some(error) = failure {
                    return self.explicit_failure(error);
                }
                Ok(GpuRouteDecision {
                    target: GpuRouteTarget::Gpu {
                        device_id: selected.expect("checked above").id,
                    },
                    reason: GpuRouteReason::GpuSelected,
                    fallback_used: false,
                })
            }
        }
    }

    fn route_auto(&self, workload: GpuWorkload) -> GpuRouteDecision {
        let cpu = |reason| GpuRouteDecision {
            target: GpuRouteTarget::Cpu,
            reason,
            fallback_used: false,
        };
        let Some(info) = self.inventory.available().find(|info| {
            (workload.precision == GpuPrecision::F32 || info.supports_f64)
                && (info.memory_budget_bytes == 0 || workload.gpu_bytes <= info.memory_budget_bytes)
        }) else {
            return if self.inventory.available().next().is_some() {
                if workload.precision == GpuPrecision::F64 {
                    cpu(GpuRouteReason::AutoPrecisionUnsupported)
                } else {
                    cpu(GpuRouteReason::AutoMemoryBudget)
                }
            } else {
                cpu(GpuRouteReason::AutoNoDevice)
            };
        };
        if workload.batch_items < self.policy.min_batch_items {
            return cpu(GpuRouteReason::AutoBelowThreshold);
        }
        let cpu_ns = workload.cpu_ns as f64;
        let gpu_ns = workload.gpu.total_ns(workload.resident) as f64;
        if cpu_ns == 0.0 || gpu_ns >= cpu_ns * (1.0 - self.policy.min_net_speedup) {
            return cpu(GpuRouteReason::AutoNoNetBenefit);
        }
        GpuRouteDecision {
            target: GpuRouteTarget::Gpu { device_id: info.id },
            reason: GpuRouteReason::GpuSelected,
            fallback_used: false,
        }
    }

    fn explicit_failure(&self, error: GpuError) -> Result<GpuRouteDecision, GpuError> {
        if self.policy.fallback == GpuFallbackPolicy::Cpu {
            Ok(GpuRouteDecision {
                target: GpuRouteTarget::Cpu,
                reason: GpuRouteReason::FallbackToCpu,
                fallback_used: true,
            })
        } else {
            Err(error)
        }
    }
}

/// Source tag for the observability values. Proxy metrics are suitable for break-even planning,
/// but are never represented as measured GPU timings.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuMetricSource {
    Measured,
    Proxy,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpuMetricValue {
    pub nanos: u64,
    pub source: GpuMetricSource,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpuMetricsSnapshot {
    pub source: GpuMetricSource,
    pub cold_init: GpuMetricValue,
    pub h2d: GpuMetricValue,
    pub kernel: GpuMetricValue,
    pub d2h: GpuMetricValue,
}

/// Low-cardinality stage metrics. A metrics instance has one source tag so dashboards cannot
/// accidentally aggregate proxy and hardware measurements as if they were interchangeable.
#[derive(Debug)]
pub struct GpuMetrics {
    source: GpuMetricSource,
    cold_init: AtomicU64,
    h2d: AtomicU64,
    kernel: AtomicU64,
    d2h: AtomicU64,
}

impl GpuMetrics {
    pub fn new(source: GpuMetricSource) -> Self {
        Self {
            source,
            cold_init: AtomicU64::new(0),
            h2d: AtomicU64::new(0),
            kernel: AtomicU64::new(0),
            d2h: AtomicU64::new(0),
        }
    }

    pub fn observe(&self, timings: GpuTimings) {
        self.cold_init
            .fetch_add(timings.cold_init_ns, Ordering::Relaxed);
        self.h2d.fetch_add(timings.h2d_ns, Ordering::Relaxed);
        self.kernel.fetch_add(timings.kernel_ns, Ordering::Relaxed);
        self.d2h.fetch_add(timings.d2h_ns, Ordering::Relaxed);
    }

    pub fn snapshot(&self) -> GpuMetricsSnapshot {
        GpuMetricsSnapshot {
            source: self.source,
            cold_init: GpuMetricValue {
                nanos: self.cold_init.load(Ordering::Relaxed),
                source: self.source,
            },
            h2d: GpuMetricValue {
                nanos: self.h2d.load(Ordering::Relaxed),
                source: self.source,
            },
            kernel: GpuMetricValue {
                nanos: self.kernel.load(Ordering::Relaxed),
                source: self.source,
            },
            d2h: GpuMetricValue {
                nanos: self.d2h.load(Ordering::Relaxed),
                source: self.source,
            },
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpuQueueSnapshot {
    pub queued: usize,
    pub memory_in_use: u64,
    pub queue_capacity: usize,
    pub memory_budget_bytes: u64,
}

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum GpuQueueError {
    #[error("GPU device queue is not registered")]
    UnknownDevice,
    #[error("GPU device queue is full")]
    QueueFull,
    #[error("GPU job exceeds the per-job memory budget")]
    JobMemory,
    #[error("GPU device memory budget exceeded")]
    MemoryBudget,
}

#[derive(Debug, Clone, Copy)]
pub struct GpuDeviceQueueConfig {
    pub queue_capacity: usize,
    pub memory_budget_bytes: u64,
    pub max_job_memory_bytes: u64,
}

impl Default for GpuDeviceQueueConfig {
    fn default() -> Self {
        Self {
            queue_capacity: 32,
            memory_budget_bytes: 1 << 30,
            max_job_memory_bytes: 256 << 20,
        }
    }
}

#[derive(Debug)]
struct QueueState {
    config: GpuDeviceQueueConfig,
    queued: usize,
    memory_in_use: u64,
}

/// Bounded queue and resident-memory admission, one independent budget per device.
#[derive(Debug, Clone, Default)]
pub struct GpuScheduler {
    queues: Arc<Mutex<BTreeMap<u32, QueueState>>>,
}

impl GpuScheduler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register_device(
        &self,
        device_id: u32,
        config: GpuDeviceQueueConfig,
    ) -> Result<(), GpuQueueError> {
        if config.queue_capacity == 0
            || config.memory_budget_bytes == 0
            || config.max_job_memory_bytes == 0
        {
            return Err(GpuQueueError::MemoryBudget);
        }
        if config.max_job_memory_bytes > config.memory_budget_bytes {
            return Err(GpuQueueError::JobMemory);
        }
        self.queues.lock().expect("GPU queue lock").insert(
            device_id,
            QueueState {
                config,
                queued: 0,
                memory_in_use: 0,
            },
        );
        Ok(())
    }

    pub fn snapshot(&self, device_id: u32) -> Result<GpuQueueSnapshot, GpuQueueError> {
        let queues = self.queues.lock().expect("GPU queue lock");
        let queue = queues.get(&device_id).ok_or(GpuQueueError::UnknownDevice)?;
        Ok(GpuQueueSnapshot {
            queued: queue.queued,
            memory_in_use: queue.memory_in_use,
            queue_capacity: queue.config.queue_capacity,
            memory_budget_bytes: queue.config.memory_budget_bytes,
        })
    }

    pub fn try_acquire(
        &self,
        device_id: u32,
        memory_bytes: u64,
    ) -> Result<GpuPermit, GpuQueueError> {
        let mut queues = self.queues.lock().expect("GPU queue lock");
        let queue = queues
            .get_mut(&device_id)
            .ok_or(GpuQueueError::UnknownDevice)?;
        if memory_bytes > queue.config.max_job_memory_bytes {
            return Err(GpuQueueError::JobMemory);
        }
        if queue.queued >= queue.config.queue_capacity {
            return Err(GpuQueueError::QueueFull);
        }
        if memory_bytes
            > queue
                .config
                .memory_budget_bytes
                .saturating_sub(queue.memory_in_use)
        {
            return Err(GpuQueueError::MemoryBudget);
        }
        queue.queued += 1;
        queue.memory_in_use += memory_bytes;
        Ok(GpuPermit {
            scheduler: self.clone(),
            device_id,
            memory_bytes,
            released: false,
        })
    }

    fn release(&self, device_id: u32, memory_bytes: u64) {
        if let Some(queue) = self
            .queues
            .lock()
            .expect("GPU queue lock")
            .get_mut(&device_id)
        {
            queue.queued = queue.queued.saturating_sub(1);
            queue.memory_in_use = queue.memory_in_use.saturating_sub(memory_bytes);
        }
    }
}

#[derive(Debug)]
pub struct GpuPermit {
    scheduler: GpuScheduler,
    device_id: u32,
    memory_bytes: u64,
    released: bool,
}

impl GpuPermit {
    pub fn release(mut self) {
        if !self.released {
            self.scheduler.release(self.device_id, self.memory_bytes);
            self.released = true;
        }
    }
}

impl Drop for GpuPermit {
    fn drop(&mut self) {
        if !self.released {
            self.scheduler.release(self.device_id, self.memory_bytes);
            self.released = true;
        }
    }
}

/// Result of a real (feature-gated) Burn probe. On CPU-only builds this reports the reason for
/// unavailability instead of attempting to initialize WGPU.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GpuProbe {
    pub compiled: bool,
    pub available: bool,
    pub supports_f64: bool,
    pub label: String,
    pub timings: GpuTimings,
    pub source: GpuMetricSource,
    pub reason: Option<String>,
}

#[cfg(feature = "gpu")]
pub fn probe_wgpu(device_id: Option<u32>) -> GpuProbe {
    use burn::tensor::backend::Backend;
    use burn::tensor::{DType, Tensor, TensorData};
    use engine_core::backend::GpuBackend;
    use std::panic::{catch_unwind, AssertUnwindSafe};

    let started = Instant::now();
    let attempt = catch_unwind(AssertUnwindSafe(|| {
        type Device = burn::tensor::Device<GpuBackend>;
        let device: Device = match device_id {
            Some(id) => burn::backend::wgpu::WgpuDevice::DiscreteGpu(id as usize),
            None => Default::default(),
        };
        let supports_f64 = GpuBackend::supports_dtype(&device, DType::F64);
        if !supports_f64 {
            return Ok((
                supports_f64,
                GpuTimings {
                    cold_init_ns: started.elapsed().as_nanos() as u64,
                    ..GpuTimings::default()
                },
            ));
        }
        let h2d = Instant::now();
        let a: Tensor<GpuBackend, 1> = Tensor::from_data(TensorData::from([1.0f64]), &device);
        let h2d_ns = h2d.elapsed().as_nanos() as u64;
        let kernel = Instant::now();
        let b = a.clone() + a;
        let kernel_ns = kernel.elapsed().as_nanos() as u64;
        let d2h = Instant::now();
        let result = b
            .into_data()
            .to_vec::<f64>()
            .map_err(|error| error.to_string())?;
        let d2h_ns = d2h.elapsed().as_nanos() as u64;
        if result != vec![2.0] {
            return Err("Burn WGPU probe returned an unexpected result".to_owned());
        }
        Ok((
            supports_f64,
            GpuTimings {
                cold_init_ns: started.elapsed().as_nanos() as u64,
                h2d_ns,
                kernel_ns,
                d2h_ns,
            },
        ))
    }));
    match attempt {
        Ok(Ok((supports_f64, timings))) => GpuProbe {
            compiled: true,
            available: true,
            supports_f64,
            label: format!(
                "wgpu device {}",
                device_id.map_or_else(|| "default".into(), |id| id.to_string())
            ),
            timings,
            source: GpuMetricSource::Measured,
            reason: None,
        },
        Ok(Err(reason)) => GpuProbe {
            compiled: true,
            available: false,
            supports_f64: false,
            label: "wgpu unavailable".into(),
            timings: GpuTimings {
                cold_init_ns: started.elapsed().as_nanos() as u64,
                ..GpuTimings::default()
            },
            source: GpuMetricSource::Measured,
            reason: Some(reason),
        },
        Err(_) => GpuProbe {
            compiled: true,
            available: false,
            supports_f64: false,
            label: "wgpu unavailable".into(),
            timings: GpuTimings {
                cold_init_ns: started.elapsed().as_nanos() as u64,
                ..GpuTimings::default()
            },
            source: GpuMetricSource::Measured,
            reason: Some(
                "WGPU initialization panicked (no usable adapter or unsupported device)".into(),
            ),
        },
    }
}

#[cfg(not(feature = "gpu"))]
pub fn probe_wgpu(_device_id: Option<u32>) -> GpuProbe {
    GpuProbe {
        compiled: false,
        available: false,
        supports_f64: false,
        label: "gpu feature disabled".into(),
        timings: GpuTimings::default(),
        source: GpuMetricSource::Proxy,
        reason: Some("compile quant-engine with --features gpu".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn workload(batch_items: usize) -> GpuWorkload {
        GpuWorkload {
            batch_items,
            gpu_bytes: 256,
            precision: GpuPrecision::F64,
            resident: false,
            cpu_ns: 100_000,
            gpu: GpuTimings {
                cold_init_ns: 1_000,
                h2d_ns: 2_000,
                kernel_ns: 10_000,
                d2h_ns: 2_000,
            },
        }
    }

    #[test]
    fn device_unavailable_is_visible_and_auto_stays_cpu() {
        let router = GpuRouter::new(
            GpuInventory::default(),
            GpuRoutingPolicy {
                min_batch_items: 1,
                ..Default::default()
            },
        );
        let decision = router.route(DevicePreference::Auto, workload(10)).unwrap();
        assert_eq!(decision.target, GpuRouteTarget::Cpu);
        assert_eq!(decision.reason, GpuRouteReason::AutoNoDevice);
        assert!(!decision.fallback_used);
        assert_eq!(
            router.route(DevicePreference::Gpu { device: None }, workload(10)),
            Err(GpuError::DeviceUnavailable)
        );
    }

    #[test]
    fn explicit_missing_device_is_not_replaced_by_another_gpu() {
        let router = GpuRouter::new(
            GpuInventory::new([GpuDeviceInfo::test_device(0, true, 1 << 20)]),
            GpuRoutingPolicy::default(),
        );
        assert_eq!(
            router.route(DevicePreference::Gpu { device: Some(7) }, workload(2)),
            Err(GpuError::DeviceUnavailable)
        );
    }

    #[test]
    fn explicit_gpu_fallback_is_opt_in_and_recorded() {
        let router = GpuRouter::new(
            GpuInventory::new([GpuDeviceInfo::test_device(0, false, 1 << 20)]),
            GpuRoutingPolicy {
                fallback: GpuFallbackPolicy::Cpu,
                ..Default::default()
            },
        );
        let decision = router
            .route(DevicePreference::Gpu { device: Some(0) }, workload(10))
            .unwrap();
        assert_eq!(decision.target, GpuRouteTarget::Cpu);
        assert_eq!(decision.reason, GpuRouteReason::FallbackToCpu);
        assert!(decision.fallback_used);
    }

    #[test]
    fn auto_threshold_and_net_benefit_are_both_required() {
        let router = GpuRouter::new(
            GpuInventory::new([GpuDeviceInfo::test_device(0, true, 1 << 20)]),
            GpuRoutingPolicy {
                min_batch_items: 100,
                min_net_speedup: 0.05,
                ..Default::default()
            },
        );
        assert_eq!(
            router
                .route(DevicePreference::Auto, workload(10))
                .unwrap()
                .reason,
            GpuRouteReason::AutoBelowThreshold
        );
        let mut slow = workload(100);
        slow.gpu.kernel_ns = 99_000;
        assert_eq!(
            router.route(DevicePreference::Auto, slow).unwrap().reason,
            GpuRouteReason::AutoNoNetBenefit
        );
        assert_eq!(
            router
                .route(DevicePreference::Auto, workload(100))
                .unwrap()
                .target,
            GpuRouteTarget::Gpu { device_id: 0 }
        );
    }

    #[test]
    fn queue_is_bounded_per_device_and_releases_memory() {
        let scheduler = GpuScheduler::new();
        scheduler
            .register_device(
                0,
                GpuDeviceQueueConfig {
                    queue_capacity: 2,
                    memory_budget_bytes: 100,
                    max_job_memory_bytes: 80,
                },
            )
            .unwrap();
        scheduler
            .register_device(
                1,
                GpuDeviceQueueConfig {
                    queue_capacity: 1,
                    memory_budget_bytes: 100,
                    max_job_memory_bytes: 80,
                },
            )
            .unwrap();
        let first = scheduler.try_acquire(0, 80).unwrap();
        assert!(matches!(
            scheduler.try_acquire(0, 30),
            Err(GpuQueueError::MemoryBudget)
        ));
        assert!(scheduler.try_acquire(1, 80).is_ok());
        drop(first);
        assert_eq!(scheduler.snapshot(0).unwrap().memory_in_use, 0);
        assert!(scheduler.try_acquire(0, 80).is_ok());
    }

    #[test]
    fn metrics_keep_proxy_source_tag() {
        let metrics = GpuMetrics::new(GpuMetricSource::Proxy);
        metrics.observe(GpuTimings {
            h2d_ns: 2,
            kernel_ns: 3,
            d2h_ns: 4,
            cold_init_ns: 1,
        });
        let snapshot = metrics.snapshot();
        assert_eq!(snapshot.source, GpuMetricSource::Proxy);
        assert_eq!(snapshot.kernel.source, GpuMetricSource::Proxy);
        assert_eq!(snapshot.kernel.nanos, 3);
    }

    #[cfg(feature = "gpu")]
    #[test]
    fn gpu_probe_reports_skip_without_failing_when_hardware_is_absent() {
        let probe = probe_wgpu(None);
        if !probe.available {
            eprintln!("GPU test skipped: {}", probe.reason.unwrap_or_default());
        }
    }

    #[cfg(feature = "gpu")]
    #[test]
    fn cpu_gpu_f64_probe_agrees_and_reports_warm_measurement() {
        let cold = probe_wgpu(None);
        if !cold.available || !cold.supports_f64 {
            eprintln!(
                "CPU/GPU test skipped: {}",
                cold.reason.unwrap_or_else(|| "f64 unsupported".into())
            );
            return;
        }
        use burn::tensor::{Tensor, TensorData};
        use engine_core::backend::CpuBackend;
        type Device = burn::tensor::Device<CpuBackend>;
        let device = Device::default();
        let cpu: Tensor<CpuBackend, 1> = Tensor::from_data(TensorData::from([1.0f64]), &device);
        let cpu_result = (cpu.clone() + cpu).into_data().to_vec::<f64>().unwrap();
        assert_eq!(cpu_result, vec![2.0]);
        let warm = probe_wgpu(None);
        assert!(warm.available);
        assert!(warm.supports_f64);
        eprintln!(
            "CPU/GPU f64 probe: cold_init_ns={} warm_init_ns={} cpu_result={:?} gpu_result=2.0",
            cold.timings.cold_init_ns, warm.timings.cold_init_ns, cpu_result
        );
    }
}
