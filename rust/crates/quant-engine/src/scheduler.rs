//! CPU scheduler primitives used by the production engine.
//!
//! The scheduler deliberately owns admission rather than relying on Tokio's blocking pool.
//! `cpu_units` and `estimated_bytes` are budgets, not measurements: callers must provide a
//! conservative estimate before a job enters the bounded queue.

use std::sync::{Arc, Condvar, Mutex};

/// Resource budget charged to a job while it is queued or executing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct JobCost {
    /// Number of logical CPU slots requested by the job. A value of one is the safe default for
    /// kernels that parallelise internally only when explicitly configured.
    pub cpu_units: u32,
    /// Conservative resident-memory estimate used for load shedding.
    pub estimated_bytes: u64,
    /// Reserved for a future GPU queue; it is kept in the public contract now so admission does
    /// not have to change when GPU work is added.
    pub gpu_bytes: u64,
}

impl JobCost {
    pub fn new(cpu_units: u32, estimated_bytes: u64) -> Self {
        Self {
            cpu_units: cpu_units.max(1),
            estimated_bytes,
            gpu_bytes: 0,
        }
    }

    pub fn with_gpu_bytes(mut self, gpu_bytes: u64) -> Self {
        self.gpu_bytes = gpu_bytes;
        self
    }
}

/// Explicit policy for nested parallelism. The default is conservative: one scheduler slot per
/// job and no nested Rayon/OpenMP/BLAS parallelism. Providers may opt in only when their
/// descriptor and deployment configuration account for the inner thread count.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum NestedParallelism {
    #[default]
    Disabled,
    Enabled,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AdmissionLimits {
    pub cpu_units: u32,
    pub memory_bytes: u64,
}

impl AdmissionLimits {
    pub fn new(cpu_units: u32, memory_bytes: u64) -> Self {
        Self {
            cpu_units: cpu_units.max(1),
            memory_bytes,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AdmissionSnapshot {
    pub cpu_in_use: u32,
    pub memory_in_use: u64,
    pub queued: usize,
}

#[derive(Debug)]
struct AdmissionState {
    limits: AdmissionLimits,
    cpu_in_use: u32,
    memory_in_use: u64,
    queued: usize,
}

/// Weighted semaphore for CPU and memory. `try_acquire` is intentionally non-blocking so an HTTP
/// request can return 429/503 before RAM is exhausted. `acquire` is useful for a trusted producer
/// that wants to wait without spinning.
#[derive(Debug)]
pub struct WeightedAdmission {
    state: Mutex<AdmissionState>,
    wake: Condvar,
}

impl WeightedAdmission {
    pub fn new(limits: AdmissionLimits) -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(AdmissionState {
                limits,
                cpu_in_use: 0,
                memory_in_use: 0,
                queued: 0,
            }),
            wake: Condvar::new(),
        })
    }

    pub fn limits(&self) -> AdmissionLimits {
        self.state.lock().expect("admission lock").limits
    }

    pub fn snapshot(&self) -> AdmissionSnapshot {
        let state = self.state.lock().expect("admission lock");
        AdmissionSnapshot {
            cpu_in_use: state.cpu_in_use,
            memory_in_use: state.memory_in_use,
            queued: state.queued,
        }
    }

    pub fn try_acquire(self: &Arc<Self>, cost: JobCost) -> Option<AdmissionPermit> {
        let mut state = self.state.lock().expect("admission lock");
        if !fits(&state, cost) {
            return None;
        }
        charge(&mut state, cost);
        state.queued += 1;
        Some(AdmissionPermit {
            admission: Arc::clone(self),
            cost,
            released: false,
        })
    }

    pub fn acquire(self: &Arc<Self>, cost: JobCost) -> AdmissionPermit {
        let mut state = self.state.lock().expect("admission lock");
        while !fits(&state, cost) {
            state = self.wake.wait(state).expect("admission lock");
        }
        charge(&mut state, cost);
        state.queued += 1;
        AdmissionPermit {
            admission: Arc::clone(self),
            cost,
            released: false,
        }
    }

    fn release(&self, cost: JobCost) {
        let mut state = self.state.lock().expect("admission lock");
        state.cpu_in_use = state.cpu_in_use.saturating_sub(cost.cpu_units);
        state.memory_in_use = state.memory_in_use.saturating_sub(cost.estimated_bytes);
        state.queued = state.queued.saturating_sub(1);
        self.wake.notify_all();
    }
}

fn fits(state: &AdmissionState, cost: JobCost) -> bool {
    cost.cpu_units <= state.limits.cpu_units.saturating_sub(state.cpu_in_use)
        && cost.estimated_bytes
            <= state
                .limits
                .memory_bytes
                .saturating_sub(state.memory_in_use)
}

fn charge(state: &mut AdmissionState, cost: JobCost) {
    state.cpu_in_use += cost.cpu_units;
    state.memory_in_use += cost.estimated_bytes;
}

#[derive(Debug)]
pub struct AdmissionPermit {
    admission: Arc<WeightedAdmission>,
    cost: JobCost,
    released: bool,
}

impl AdmissionPermit {
    /// Release the budget before the permit is dropped. This is useful when the queue entry is
    /// accepted but the worker has already completed the job.
    pub fn release(mut self) {
        if !self.released {
            self.admission.release(self.cost);
            self.released = true;
        }
    }
}

impl Drop for AdmissionPermit {
    fn drop(&mut self) {
        if !self.released {
            self.admission.release(self.cost);
            self.released = true;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn weighted_admission_sheds_overload_and_releases() {
        let admission = WeightedAdmission::new(AdmissionLimits::new(2, 100));
        let permit = admission.try_acquire(JobCost::new(2, 80)).unwrap();
        assert!(admission.try_acquire(JobCost::new(1, 1)).is_none());
        assert_eq!(admission.snapshot().cpu_in_use, 2);
        permit.release();
        assert!(admission.try_acquire(JobCost::new(1, 1)).is_some());
    }

    #[test]
    fn oversized_memory_is_rejected_without_waiting() {
        let admission = WeightedAdmission::new(AdmissionLimits::new(1, 8));
        assert!(admission.try_acquire(JobCost::new(1, 9)).is_none());
        assert_eq!(admission.snapshot().memory_in_use, 0);
    }
}
