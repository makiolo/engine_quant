//! CPU vector strategy for the Phase-7 numeric reduction kernel.
//!
//! The risk planner's portfolio base value is a contiguous `f64` reduction.  This module keeps
//! that kernel independent from Burn devices: SIMD is a CPU execution strategy, never a compute
//! backend.  AVX2 is compiled into a function with an explicit `target_feature` and is entered
//! only after runtime detection.  Unsupported requests therefore remain correct on every CPU.
//!
//! The AVX2 reduction intentionally does not use FMA.  Reassociation may change a finite result
//! by a few ULPs, so callers that require bit-for-bit left-to-right IEEE-754 accumulation can use
//! `CpuVectorPolicy::Scalar` (the default deterministic policy remains available to them).

use quant_domain::CpuVectorPolicy;

/// CPU capabilities relevant to the currently implemented vector strategy.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CpuVectorCapabilities {
    /// x86 AVX2 packed `f64` arithmetic and unaligned loads.
    pub avx2: bool,
    /// FMA is reported for observability/future kernels; the sum kernel does not use it.
    pub fma: bool,
    /// AVX-512F is reported but has no implementation in Phase 7.
    pub avx512f: bool,
}

impl CpuVectorCapabilities {
    /// Detect features without executing any unsupported instruction.
    pub fn detect() -> Self {
        #[cfg(target_arch = "x86_64")]
        {
            Self {
                avx2: std::is_x86_feature_detected!("avx2"),
                fma: std::is_x86_feature_detected!("fma"),
                avx512f: std::is_x86_feature_detected!("avx512f"),
            }
        }
        #[cfg(not(target_arch = "x86_64"))]
        {
            Self {
                avx2: false,
                fma: false,
                avx512f: false,
            }
        }
    }

    /// A capability set useful for deterministic fallback tests and embedders that want to
    /// model a constrained deployment.
    pub const fn scalar_only() -> Self {
        Self {
            avx2: false,
            fma: false,
            avx512f: false,
        }
    }
}

/// Effective implementation selected after policy and runtime capability resolution.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CpuVectorStrategy {
    Scalar,
    Avx2,
}

/// Resolve a public policy against explicit capabilities.
///
/// `Auto` is deliberately conservative in production: the Phase-7 end-to-end gate was not met
/// reliably on the local risk workload, so it stays scalar until a caller opts into an explicit
/// threshold with [`resolve_cpu_vector_policy_with_auto_threshold`]. `Avx2` remains an explicit
/// opt-in and still falls back when unsupported. `PortableSimd` and `Avx512` are scalar because
/// there is no stable implementation for them in this phase.
pub fn resolve_cpu_vector_policy(
    policy: &CpuVectorPolicy,
    capabilities: CpuVectorCapabilities,
    len: usize,
) -> CpuVectorStrategy {
    resolve_cpu_vector_policy_with_auto_threshold(policy, capabilities, len, None)
}

/// Resolve a policy while explicitly opting into the experimental Auto threshold.
pub fn resolve_cpu_vector_policy_with_auto_threshold(
    policy: &CpuVectorPolicy,
    capabilities: CpuVectorCapabilities,
    len: usize,
    auto_min_len: Option<usize>,
) -> CpuVectorStrategy {
    match policy {
        CpuVectorPolicy::Scalar | CpuVectorPolicy::PortableSimd | CpuVectorPolicy::Avx512 => {
            CpuVectorStrategy::Scalar
        }
        CpuVectorPolicy::Avx2 => {
            if capabilities.avx2 && len >= 4 {
                CpuVectorStrategy::Avx2
            } else {
                CpuVectorStrategy::Scalar
            }
        }
        CpuVectorPolicy::Auto => {
            if auto_min_len.is_some_and(|min_len| capabilities.avx2 && len >= min_len) {
                CpuVectorStrategy::Avx2
            } else {
                CpuVectorStrategy::Scalar
            }
        }
    }
}

/// Sum a finite or non-finite `f64` slice using the requested CPU policy.
///
/// Non-finite input deliberately takes the scalar path.  That preserves the reference's
/// left-to-right NaN/Infinity behavior, including `+Inf + -Inf -> NaN`, instead of allowing a
/// reordered horizontal reduction to obscure where a non-finite value entered the portfolio.
pub fn sum_f64(values: &[f64], policy: &CpuVectorPolicy) -> f64 {
    if values.iter().any(|value| !value.is_finite()) {
        return scalar_sum(values);
    }
    let strategy = resolve_cpu_vector_policy(policy, CpuVectorCapabilities::detect(), values.len());
    sum_f64_with_strategy(values, strategy)
}

/// Test/benchmark hook that avoids changing process-wide CPU detection state.
pub fn sum_f64_with_capabilities(
    values: &[f64],
    policy: &CpuVectorPolicy,
    capabilities: CpuVectorCapabilities,
) -> f64 {
    if values.iter().any(|value| !value.is_finite()) {
        return scalar_sum(values);
    }
    let strategy = resolve_cpu_vector_policy(policy, capabilities, values.len());
    sum_f64_with_strategy(values, strategy)
}

fn sum_f64_with_strategy(values: &[f64], strategy: CpuVectorStrategy) -> f64 {
    match strategy {
        CpuVectorStrategy::Scalar => scalar_sum(values),
        CpuVectorStrategy::Avx2 => {
            #[cfg(target_arch = "x86_64")]
            {
                // `resolve_cpu_vector_policy` only returns Avx2 after detection.  Keep this
                // second guard local to the unsafe boundary so future callers cannot accidentally
                // turn a new code path into an illegal-instruction crash.
                if std::is_x86_feature_detected!("avx2") {
                    // SAFETY: runtime detection above establishes AVX2 support for this thread;
                    // the callee has no assumptions about alignment and handles its tail safely.
                    return unsafe { sum_avx2(values) };
                }
            }
            scalar_sum(values)
        }
    }
}

#[inline]
fn scalar_sum(values: &[f64]) -> f64 {
    values.iter().copied().sum()
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn sum_avx2(values: &[f64]) -> f64 {
    use std::arch::x86_64::{__m256d, _mm256_add_pd, _mm256_loadu_pd, _mm256_storeu_pd};

    let chunks = values.len() / 4;
    let mut accumulator: __m256d = std::arch::x86_64::_mm256_setzero_pd();
    for chunk in 0..chunks {
        // `loadu` accepts aligned and unaligned slices.  The pointer is in-bounds because the
        // chunk count is derived from the slice length and four f64 values are loaded.
        let ptr = values.as_ptr().add(chunk * 4);
        accumulator = _mm256_add_pd(accumulator, _mm256_loadu_pd(ptr));
    }
    let mut lanes = [0.0_f64; 4];
    _mm256_storeu_pd(lanes.as_mut_ptr(), accumulator);
    let mut result = lanes.iter().copied().sum::<f64>();
    for value in &values[chunks * 4..] {
        result += *value;
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_close(left: f64, right: f64) {
        let scale = left.abs().max(right.abs()).max(1.0);
        assert!(
            (left - right).abs() <= 8.0 * f64::EPSILON * scale,
            "{left} != {right}"
        );
    }

    #[test]
    fn scalar_and_avx2_agree_for_tails_and_small_inputs() {
        let capabilities = CpuVectorCapabilities {
            avx2: true,
            fma: true,
            avx512f: false,
        };
        for len in 0..=37 {
            let values: Vec<f64> = (0..len).map(|i| (i as f64 + 0.25) / 3.0).collect();
            let scalar = sum_f64_with_capabilities(&values, &CpuVectorPolicy::Scalar, capabilities);
            let vector = sum_f64_with_capabilities(&values, &CpuVectorPolicy::Avx2, capabilities);
            assert_close(scalar, vector);
        }
    }

    #[test]
    fn finite_reduction_accepts_unaligned_slice() {
        let values: Vec<f64> = (0..21).map(|i| i as f64).collect();
        let scalar = sum_f64(&values[1..], &CpuVectorPolicy::Scalar);
        let vector = sum_f64_with_capabilities(
            &values[1..],
            &CpuVectorPolicy::Avx2,
            CpuVectorCapabilities {
                avx2: true,
                fma: false,
                avx512f: false,
            },
        );
        assert_close(scalar, vector);
    }

    #[test]
    fn non_finite_values_use_reference_semantics() {
        let values = [1.0, f64::INFINITY, -f64::INFINITY, 3.0];
        assert!(sum_f64_with_capabilities(
            &values,
            &CpuVectorPolicy::Avx2,
            CpuVectorCapabilities {
                avx2: true,
                fma: true,
                avx512f: true
            },
        )
        .is_nan());
        let nan_values = [1.0, f64::NAN, 3.0];
        assert!(sum_f64(&nan_values, &CpuVectorPolicy::Auto).is_nan());
    }

    #[test]
    fn unsupported_forced_policy_falls_back_to_scalar() {
        let values = [1.0, 2.0, 3.0, 4.0];
        let scalar = sum_f64_with_capabilities(
            &values,
            &CpuVectorPolicy::Scalar,
            CpuVectorCapabilities::scalar_only(),
        );
        let forced = sum_f64_with_capabilities(
            &values,
            &CpuVectorPolicy::Avx2,
            CpuVectorCapabilities::scalar_only(),
        );
        assert_eq!(scalar, forced);
        assert_eq!(
            resolve_cpu_vector_policy(
                &CpuVectorPolicy::Avx512,
                CpuVectorCapabilities {
                    avx2: true,
                    fma: true,
                    avx512f: true
                },
                1
            ),
            CpuVectorStrategy::Scalar
        );
    }

    #[test]
    fn auto_is_scalar_until_an_explicit_threshold_is_opted_in() {
        let capabilities = CpuVectorCapabilities {
            avx2: true,
            fma: true,
            avx512f: false,
        };
        assert_eq!(
            resolve_cpu_vector_policy(&CpuVectorPolicy::Auto, capabilities, 65_536),
            CpuVectorStrategy::Scalar
        );
        assert_eq!(
            resolve_cpu_vector_policy_with_auto_threshold(
                &CpuVectorPolicy::Auto,
                capabilities,
                65_536,
                Some(16)
            ),
            CpuVectorStrategy::Avx2
        );
    }
}
