//! Fase 0: cadena de humo del pipeline de build (Rust -> cxx -> C++ -> nanobind -> Python).
//! Los kernels numéricos y el trait `ComputeBackend` (PLAN.md §5.1) llegan en Fase 1.

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
