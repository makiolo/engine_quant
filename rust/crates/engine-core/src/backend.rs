//! Abstracción de backend de cómputo (PLAN.md §5.1): la capa C++ y los clientes nunca
//! hablan con un backend concreto, solo con el trait [`ComputeBackend`]. Este módulo
//! implementa el primer backend, `CpuBackend` (rayon + SIMD portable vía `wide`), que
//! sirve de referencia funcional y de correctitud antes de añadir un backend GPU (Fase 5).

use crate::scalar::Scalar;
use rand::SeedableRng;
use rand_chacha::ChaCha8Rng;
use rand_distr::{Distribution, StandardNormal};
use rayon::prelude::*;
use wide::f64x4;

/// Operaciones vectoriales que cualquier backend de cómputo debe ofrecer: generación de
/// paths/shocks aleatorios, evaluación de payoffs vectorizada y reducciones/agregaciones.
pub trait ComputeBackend: Send + Sync {
    /// `n_paths` trayectorias independientes de `n_steps` shocks normales estándar cada
    /// una, deterministas a partir de `seed` (mismo `seed` ⇒ mismos shocks, necesario
    /// para los tests de capa 1-2 de PLAN.md §5.6). Cada trayectoria usa su propio
    /// stream de RNG (derivado de `seed` y el índice de path) para poder generarse en
    /// paralelo sin dependencias entre paths.
    fn normal_shocks(&self, n_paths: usize, n_steps: usize, seed: u64) -> Vec<Vec<f64>>;

    /// Evalúa `f` sobre cada elemento de `inputs` en paralelo. Genérica sobre `T: Scalar`
    /// para poder vectorizar tanto valoración pura (`T = f64`) como sensibilidades AAD
    /// (`T = Dual`).
    fn evaluate<T, F>(&self, inputs: &[T], f: F) -> Vec<T>
    where
        T: Scalar,
        F: Fn(&T) -> T + Sync;

    /// Media aritmética genérica (funciona para cualquier `T: Scalar`, sin SIMD).
    fn mean<T: Scalar>(&self, values: &[T]) -> T {
        crate::kernel::mean(values)
    }
}

/// Backend de referencia: paralelismo entre paths vía `rayon`, reducciones numéricas
/// (`f64`) vectorizadas vía SIMD portable (`wide::f64x4`).
#[derive(Debug, Default, Clone, Copy)]
pub struct CpuBackend;

impl ComputeBackend for CpuBackend {
    fn normal_shocks(&self, n_paths: usize, n_steps: usize, seed: u64) -> Vec<Vec<f64>> {
        (0..n_paths)
            .into_par_iter()
            .map(|path_idx| {
                // Mezcla simple del índice de path en la semilla (constante de Fibonacci
                // hashing) para obtener streams independientes y reproducibles por path.
                let path_seed = seed
                    .wrapping_add((path_idx as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15));
                let mut rng = ChaCha8Rng::seed_from_u64(path_seed);
                (0..n_steps)
                    .map(|_| StandardNormal.sample(&mut rng))
                    .collect()
            })
            .collect()
    }

    fn evaluate<T, F>(&self, inputs: &[T], f: F) -> Vec<T>
    where
        T: Scalar,
        F: Fn(&T) -> T + Sync,
    {
        inputs.par_iter().map(&f).collect()
    }
}

/// Media aritmética de valores `f64` vectorizada con SIMD portable (`wide::f64x4`).
/// Camino rápido usado explícitamente por la valoración pura (Monte Carlo con
/// `T = f64`); las sensibilidades AAD (`T = Dual`) usan la reducción genérica de
/// [`ComputeBackend::mean`], ya que `Dual` no es un tipo primitivo vectorizable por SIMD.
pub fn mean_f64_simd(values: &[f64]) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    let chunks = values.chunks_exact(4);
    let remainder = chunks.remainder();

    let simd_sum = chunks
        .map(|c| f64x4::from(<[f64; 4]>::try_from(c).unwrap()))
        .fold(f64x4::ZERO, |acc, v| acc + v)
        .reduce_add();
    let scalar_sum: f64 = remainder.iter().sum();

    (simd_sum + scalar_sum) / values.len() as f64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normal_shocks_are_deterministic_for_same_seed() {
        let backend = CpuBackend;
        let a = backend.normal_shocks(8, 16, 42);
        let b = backend.normal_shocks(8, 16, 42);
        assert_eq!(a, b);
    }

    #[test]
    fn normal_shocks_differ_across_paths() {
        let backend = CpuBackend;
        let shocks = backend.normal_shocks(4, 16, 7);
        assert_ne!(shocks[0], shocks[1]);
    }

    #[test]
    fn normal_shocks_have_approximately_zero_mean_and_unit_variance() {
        let backend = CpuBackend;
        let shocks = backend.normal_shocks(20_000, 1, 123);
        let flat: Vec<f64> = shocks.into_iter().map(|s| s[0]).collect();
        let m = mean_f64_simd(&flat);
        let var: f64 = flat.iter().map(|x| (x - m).powi(2)).sum::<f64>() / flat.len() as f64;
        assert!(m.abs() < 0.05, "media ~0 esperada, got {m}");
        assert!((var - 1.0).abs() < 0.05, "varianza ~1 esperada, got {var}");
    }

    #[test]
    fn evaluate_matches_sequential_map() {
        let backend = CpuBackend;
        let inputs: Vec<f64> = (0..100).map(|i| i as f64).collect();
        let got = backend.evaluate(&inputs, |x| x * x);
        let expected: Vec<f64> = inputs.iter().map(|x| x * x).collect();
        assert_eq!(got, expected);
    }

    #[test]
    fn mean_f64_simd_matches_naive_mean_for_various_lengths() {
        for len in [0usize, 1, 3, 4, 5, 7, 8, 17] {
            let values: Vec<f64> = (0..len).map(|i| i as f64 + 1.0).collect();
            let naive = if values.is_empty() {
                0.0
            } else {
                values.iter().sum::<f64>() / values.len() as f64
            };
            let simd = mean_f64_simd(&values);
            assert!(
                (simd - naive).abs() < 1e-9,
                "len={len}: simd={simd} naive={naive}"
            );
        }
    }
}
