//! `engine-core`: kernels numéricos del motor XVA (PLAN.md §3.1).
//!
//! Todo el código de valoración es genérico sobre el trait [`scalar::Scalar`], de forma
//! que puede instanciarse tanto con `f64` (valoración pura) como con `dual::Dual`
//! (sensibilidades vía AAD forward-mode, PLAN.md §5.3).

pub mod backend;
pub mod dual;
pub mod kernel;
pub mod models;
pub mod scalar;

pub fn ping() -> f64 {
    42.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ping_returns_42() {
        assert_eq!(ping(), 42.0);
    }
}
