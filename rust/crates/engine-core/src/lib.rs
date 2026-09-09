//! `engine-core`: kernels numéricos del motor XVA (PLAN.md §3.1).
//!
//! Todo el código de valoración es genérico sobre `B: burn::tensor::backend::Backend`
//! (ver `crate::backend`), de forma que la misma función sirve para valoración pura en
//! CPU o GPU y, envolviendo el backend con `Autodiff` (PLAN.md §5.3), para propagar
//! sensibilidades sin reescribir la lógica de negocio.

pub mod backend;
pub mod exposure;
pub mod kernel;
pub mod models;
pub mod products;
pub mod smoke;

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
