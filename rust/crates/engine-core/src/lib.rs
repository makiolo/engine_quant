//! `engine-core`: kernels numéricos del motor XVA (PLAN.md §3.1).
//!
//! Todo el código de valoración es genérico sobre `B: burn::tensor::backend::Backend`
//! (ver `crate::backend`), de forma que la misma función sirve para valoración pura en
//! CPU o GPU y, envolviendo el backend con `Autodiff` (PLAN.md §5.3), para propagar
//! sensibilidades sin reescribir la lógica de negocio.

pub mod api;
pub mod backend;
pub mod calibration;
pub mod curve;
pub mod exposure;
pub mod kernel;
pub mod models;
pub mod products;
pub mod smoke;

pub fn ping() -> f64 {
    42.0
}

/// Lock compartido entre TODOS los tests del crate que necesiten reproducibilidad exacta de
/// `B::seed`/`Tensor::random` (PLAN.md §7.19, descubierto al añadir los tests de lote de
/// `crate::exposure`) -- `NdArray::seed` (`burn-ndarray`, el backend de test) guarda el RNG en
/// un `Mutex` **global de proceso**, no uno por hilo: dos tests que siembran y generan
/// aleatorios *al mismo tiempo* en hilos distintos (el modo por defecto de `cargo test`) se
/// pisan el uno al otro aunque usen la misma seed, así que cualquier test que compare un
/// resultado derivado de `B::seed` bit a bit (no solo invariantes como "EE >= 0") debe adquirir
/// este lock durante todo el sembrado+simulación. No sustituye el `Mutex` interno de
/// `burn-ndarray` (privado a esa crate): es un segundo lock, de este crate, que solo protege a
/// los tests que lo adquieren -- de ahí que *todos* los tests sembrados del crate deban usarlo,
/// no solo los nuevos, o no protege a nadie.
#[cfg(test)]
pub(crate) mod rng_test_lock {
    use std::sync::Mutex;
    pub static LOCK: Mutex<()> = Mutex::new(());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ping_returns_42() {
        assert_eq!(ping(), 42.0);
    }
}
