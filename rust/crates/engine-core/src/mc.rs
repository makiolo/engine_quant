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
}
