//! Agregacion Monte Carlo generica: media, error estandar e intervalo de confianza sobre un
//! conjunto de muestras pathwise (PLAN_PRODUCTS.md §12 Fase 5: "agregacion `E_Q[discounted
//! cashflows]`, error estandar e intervalos de confianza"). Generaliza el calculo que hasta ahora
//! solo vivia inline en `models::hull_white::tests::monte_carlo_bond_price_converges_to_analytic_formula`
//! (media + `variance`/`n_paths` + `sqrt`) para que `models::gbm`/`payoff` no lo repitan.
//!
//! No depende de ninguna crate de estadistica (mismo criterio que `calibration::solve_2x2`: evitar
//! una dependencia nueva para una formula de una linea) -- el cuantil normal `z` para el intervalo
//! de confianza se pasa como parametro en vez de calcularse a partir de un nivel de confianza, así
//! que tampoco hace falta una inversa de la CDF normal aqui.

/// Resultado de agregar `n_paths` muestras independientes de un estimador Monte Carlo.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct McEstimate {
    pub mean: f64,
    /// Desviacion estandar del ESTIMADOR (`sqrt(varianza_muestral / n_paths)`), no de la muestra.
    pub std_error: f64,
    pub ci_low: f64,
    pub ci_high: f64,
    pub n_paths: u64,
}

/// `z` del intervalo de confianza normal al 95% (dos colas) -- el nivel que reportan por defecto
/// las medidas de esta fase salvo que quien llama pida otro explicitamente con `aggregate`.
pub const Z_95: f64 = 1.959_963_984_540_054;

/// Versioned counter-based RNG. Path chunks can run in any order because each variate is keyed
/// by `(seed, scenario_id, path_id, factor_id, step)` rather than by mutable global state.
pub const RNG_ALGORITHM_VERSION: &str = "splitmix64-box-muller-v1";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RngVersion {
    pub algorithm: &'static str,
}

impl Default for RngVersion {
    fn default() -> Self {
        Self {
            algorithm: RNG_ALGORITHM_VERSION,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PartitionedRng {
    seed: u64,
    scenario_id: u64,
    path_id: u64,
}

impl PartitionedRng {
    pub fn new(seed: u64, scenario_id: u64, path_id: u64) -> Self {
        Self {
            seed,
            scenario_id,
            path_id,
        }
    }

    pub fn version(&self) -> RngVersion {
        RngVersion::default()
    }

    pub fn counter(&self, factor_id: u32, step: u32) -> u64 {
        splitmix64(
            self.seed
                .wrapping_add(self.scenario_id.rotate_left(17))
                .wrapping_add(self.path_id.rotate_left(33))
                .wrapping_add((factor_id as u64) << 32)
                .wrapping_add(step as u64),
        )
    }

    pub fn uniform01(&self, factor_id: u32, step: u32) -> f64 {
        ((self.counter(factor_id, step) >> 11) as f64) * (1.0 / ((1_u64 << 53) as f64))
    }

    pub fn normal(&self, factor_id: u32, step: u32) -> f64 {
        let u1 = self.uniform01(factor_id, step).max(f64::MIN_POSITIVE);
        let u2 = self.uniform01(factor_id ^ 0x9e37_79b9, step ^ 0x7f4a_7c15);
        (-2.0 * u1.ln()).sqrt() * (std::f64::consts::TAU * u2).cos()
    }
}

fn splitmix64(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut z = value;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    z ^ (z >> 31)
}

/// Agrega `samples` (una muestra independiente por ruta) en media + error estandar + intervalo de
/// confianza `mean +/- z * std_error`. `samples` no puede estar vacio -- un Monte Carlo de cero
/// rutas no es un caso de negocio valido, es un error de quien llama.
pub fn aggregate(samples: &[f64], z: f64) -> McEstimate {
    assert!(!samples.is_empty(), "mc::aggregate: se requiere al menos una ruta");
    let n = samples.len() as f64;
    let mean = samples.iter().sum::<f64>() / n;
    let variance = if samples.len() > 1 {
        samples.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / (n - 1.0)
    } else {
        0.0
    };
    let std_error = (variance / n).sqrt();
    McEstimate {
        mean,
        std_error,
        ci_low: mean - z * std_error,
        ci_high: mean + z * std_error,
        n_paths: samples.len() as u64,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aggregate_of_constant_samples_has_zero_std_error() {
        let estimate = aggregate(&[3.0; 100], Z_95);
        assert_eq!(estimate.mean, 3.0);
        assert_eq!(estimate.std_error, 0.0);
        assert_eq!(estimate.ci_low, 3.0);
        assert_eq!(estimate.ci_high, 3.0);
        assert_eq!(estimate.n_paths, 100);
    }

    #[test]
    fn aggregate_matches_hand_computed_mean_and_std_error() {
        let samples = [1.0, 2.0, 3.0, 4.0, 5.0];
        let estimate = aggregate(&samples, Z_95);
        assert_eq!(estimate.mean, 3.0);
        // varianza muestral (n-1): ((1-3)^2+(2-3)^2+0+(4-3)^2+(5-3)^2)/4 = 10/4 = 2.5
        let expected_std_error = (2.5_f64 / 5.0).sqrt();
        assert!((estimate.std_error - expected_std_error).abs() < 1e-12);
        assert!(estimate.ci_low < estimate.mean && estimate.mean < estimate.ci_high);
    }

    #[test]
    #[should_panic(expected = "al menos una ruta")]
    fn aggregate_of_empty_samples_panics() {
        aggregate(&[], Z_95);
    }

    #[test]
    fn partitioned_rng_is_stable_when_paths_are_rechunked() {
        let left = (0..32)
            .map(|path| PartitionedRng::new(7, 3, path).normal(1, 4))
            .collect::<Vec<_>>();
        let right = (0..8)
            .flat_map(|chunk| {
                let start = chunk * 4;
                (start..start + 4)
                    .map(|path| PartitionedRng::new(7, 3, path).normal(1, 4))
                    .collect::<Vec<_>>()
            })
            .collect::<Vec<_>>();
        assert_eq!(left, right);
        assert_ne!(
            PartitionedRng::new(7, 3, 0).counter(1, 4),
            PartitionedRng::new(7, 3, 1).counter(1, 4)
        );
    }
}
