//! Ejemplo de Rust consumiendo `engine/abi.h` (PLAN.md Fase 6, §5.5/§7.13) vía FFI directa a
//! C, SIN pasar por los crates internos `engine-core`/`engine-ffi` (esos hablan con la capa
//! C++ vía `cxx`, un mecanismo distinto e interno) -- deliberado: demuestra que incluso Rust,
//! que ya tiene acceso privilegiado al motor, podría consumirlo igual que Julia/.NET/Go si
//! hiciera falta (ej. un proceso Rust separado que solo tiene el `.dll`/`.so` compilado, no
//! este repo). Las declaraciones `extern "C"` de abajo son la traducción manual a Rust de
//! `cpp/engine/include/engine/abi.h` -- lo que generaría `bindgen` automáticamente.
//!
//! `cargo run` (requiere haber compilado antes el árbol CMake de este repo, ver
//! `examples/abi/README.md`; `build.rs` busca `engine_abi.dll`/`.lib` en `ENGINE_ABI_LIB_DIR`
//! o, por defecto, en `../../../build/cpp/engine` relativo a este crate). El `.dll` (Windows)
//! debe además ser localizable en tiempo de *ejecución* -- cópialo junto al `.exe` generado o
//! añade su carpeta al `PATH` antes de ejecutar, ver el README.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};

// --- Traducción manual de engine/abi.h -----------------------------------------------------

#[repr(C)]
struct EngineModel {
    _private: [u8; 0],
}
#[repr(C)]
struct EngineProduct {
    _private: [u8; 0],
}
#[repr(C)]
struct EngineMeasure {
    _private: [u8; 0],
}

#[repr(C)]
#[allow(dead_code)]
enum EngineParamKind {
    Double = 0,
    Vector = 1,
    Bool = 2,
}

#[repr(C)]
struct EngineParam {
    key: *const c_char,
    kind: EngineParamKind,
    scalar: f64,
    values: *const f64,
    count: usize,
}

#[repr(C)]
#[derive(Default)]
struct EngineMeasureResult {
    times: *mut f64,
    primary: *mut f64,
    secondary: *mut f64,
    len: usize,
    has_scalar: c_int,
    scalar: f64,
}

extern "C" {
    fn engine_abi_version() -> c_int;

    fn engine_abi_list_models(out_names: *mut *mut *const c_char) -> usize;
    fn engine_abi_free_string_list(names: *mut *const c_char, count: usize);

    fn engine_abi_create_model(name: *const c_char, params: *const EngineParam, n_params: usize) -> *mut EngineModel;
    fn engine_abi_create_product(
        name: *const c_char,
        params: *const EngineParam,
        n_params: usize,
    ) -> *mut EngineProduct;
    fn engine_abi_create_measure(name: *const c_char) -> *mut EngineMeasure;
    fn engine_abi_free_model(model: *mut EngineModel);
    fn engine_abi_free_product(product: *mut EngineProduct);
    fn engine_abi_free_measure(measure: *mut EngineMeasure);

    fn engine_abi_evaluate(
        measure: *const EngineMeasure,
        model: *const EngineModel,
        product: *const EngineProduct,
        params: *const EngineParam,
        n_params: usize,
        out_result: *mut EngineMeasureResult,
    ) -> c_int;
    fn engine_abi_free_measure_result(result: *mut EngineMeasureResult);

    fn engine_abi_set_compute_backend(name: *const c_char) -> c_int;
    fn engine_abi_get_compute_backend(buffer: *mut c_char, buffer_len: usize) -> usize;
    fn engine_abi_is_gpu_backend_available() -> c_int;

    fn engine_abi_last_error(buffer: *mut c_char, buffer_len: usize) -> usize;
}

// --- Utilidades del ejemplo (no parte de la ABI) -------------------------------------------

fn last_error() -> String {
    let mut buffer = vec![0u8; 512];
    // Seguro: engine_abi_last_error nunca escribe más de buffer.len() bytes (ver abi.h).
    let len = unsafe { engine_abi_last_error(buffer.as_mut_ptr() as *mut c_char, buffer.len()) };
    let len = len.min(buffer.len());
    String::from_utf8_lossy(&buffer[..len]).into_owned()
}

fn scalar_param(key: &CStr, value: f64) -> EngineParam {
    EngineParam { key: key.as_ptr(), kind: EngineParamKind::Double, scalar: value, values: std::ptr::null(), count: 0 }
}

fn vector_param(key: &CStr, values: &[f64]) -> EngineParam {
    EngineParam {
        key: key.as_ptr(),
        kind: EngineParamKind::Vector,
        scalar: 0.0,
        values: values.as_ptr(),
        count: values.len(),
    }
}

fn main() {
    unsafe {
        println!("engine_abi_version() = {}", engine_abi_version());

        // --- list_models: confirma que HullWhite1F está registrado -----------------------
        let mut names_ptr: *mut *const c_char = std::ptr::null_mut();
        let count = engine_abi_list_models(&mut names_ptr);
        let names = std::slice::from_raw_parts(names_ptr, count);
        let found_hull_white =
            names.iter().any(|&p| CStr::from_ptr(p).to_str() == Ok("HullWhite1F"));
        engine_abi_free_string_list(names_ptr, count);
        assert!(found_hull_white, "HullWhite1F no aparece en engine_abi_list_models");

        // --- Caso base: IRS 5y anual a la par bajo Hull-White 1F (PLAN.md §5.2) ----------
        let (key_a, key_b, key_sigma, key_r0) =
            (c_string("a"), c_string("b"), c_string("sigma"), c_string("r0"));
        let hw_params =
            [scalar_param(&key_a, 0.1), scalar_param(&key_b, 0.03), scalar_param(&key_sigma, 0.01), scalar_param(&key_r0, 0.02)];
        let model_name = c_string("HullWhite1F");
        let model = engine_abi_create_model(model_name.as_ptr(), hw_params.as_ptr(), hw_params.len());
        if model.is_null() {
            panic!("engine_abi_create_model(HullWhite1F): {}", last_error());
        }

        let payment_times = [1.0, 2.0, 3.0, 4.0, 5.0];
        let accruals = [1.0, 1.0, 1.0, 1.0, 1.0];
        let (key_notional, key_payment_times, key_accruals) =
            (c_string("notional"), c_string("payment_times"), c_string("accruals"));
        let irs_params = [
            scalar_param(&key_notional, 1_000_000.0),
            vector_param(&key_payment_times, &payment_times),
            vector_param(&key_accruals, &accruals),
        ];
        let product_name = c_string("IRSwap");
        let product = engine_abi_create_product(product_name.as_ptr(), irs_params.as_ptr(), irs_params.len());
        if product.is_null() {
            let msg = last_error();
            engine_abi_free_model(model);
            panic!("engine_abi_create_product(IRSwap): {msg}");
        }

        // --- ExposureProfile / UnilateralCVA: mismo caso/semillas que
        // clients/excel/README.md ("Verificación manual") -- si estos números no coinciden,
        // algo se rompió en la traducción C ABI <-> engine::Registries/IMeasure. ------------
        let (key_monitoring_times, key_n_paths, key_seed, key_hazard_rate, key_recovery_rate) = (
            c_string("monitoring_times"),
            c_string("n_paths"),
            c_string("seed"),
            c_string("hazard_rate"),
            c_string("recovery_rate"),
        );

        let profile_times = [0.0, 1.0, 2.0];
        let profile_measure_name = c_string("ExposureProfile");
        let profile_measure = engine_abi_create_measure(profile_measure_name.as_ptr());
        assert!(!profile_measure.is_null(), "engine_abi_create_measure(ExposureProfile): {}", last_error());
        let profile_params =
            [vector_param(&key_monitoring_times, &profile_times), scalar_param(&key_n_paths, 5000.0), scalar_param(&key_seed, 7.0)];
        let mut profile_result = EngineMeasureResult::default();
        let rc = engine_abi_evaluate(
            profile_measure,
            model,
            product,
            profile_params.as_ptr(),
            profile_params.len(),
            &mut profile_result,
        );
        assert_eq!(rc, 0, "engine_abi_evaluate(ExposureProfile): {}", last_error());
        let ee = std::slice::from_raw_parts(profile_result.primary, profile_result.len);
        println!("ExposureProfile EE = {ee:?}  (esperado [0.0, 12862.62, 13673.53])");
        engine_abi_free_measure_result(&mut profile_result);
        engine_abi_free_measure(profile_measure);

        let cva_times = [0.0, 1.0, 2.0, 3.0];
        let cva_measure_name = c_string("UnilateralCVA");
        let cva_measure = engine_abi_create_measure(cva_measure_name.as_ptr());
        assert!(!cva_measure.is_null(), "engine_abi_create_measure(UnilateralCVA): {}", last_error());
        let cva_params = [
            vector_param(&key_monitoring_times, &cva_times),
            scalar_param(&key_n_paths, 5000.0),
            scalar_param(&key_seed, 13.0),
            scalar_param(&key_hazard_rate, 0.02),
            scalar_param(&key_recovery_rate, 0.4),
        ];
        let mut cva_result = EngineMeasureResult::default();
        let rc =
            engine_abi_evaluate(cva_measure, model, product, cva_params.as_ptr(), cva_params.len(), &mut cva_result);
        assert_eq!(rc, 0, "engine_abi_evaluate(UnilateralCVA): {}", last_error());
        println!("UnilateralCVA = {}  (esperado 426.7618244093184)", cva_result.scalar);
        engine_abi_free_measure_result(&mut cva_result);
        engine_abi_free_measure(cva_measure);

        engine_abi_free_product(product);
        engine_abi_free_model(model);

        // --- Backend de cómputo (PLAN.md §7.12), misma ABI --------------------------------
        let mut backend_buf = [0u8; 16];
        let len = engine_abi_get_compute_backend(backend_buf.as_mut_ptr() as *mut c_char, backend_buf.len());
        let backend = String::from_utf8_lossy(&backend_buf[..len.min(backend_buf.len())]);
        println!(
            "backend: {backend}  (gpu disponible: {})",
            if engine_abi_is_gpu_backend_available() != 0 { "si" } else { "no" }
        );
        let cpu_name = c_string("cpu");
        assert_eq!(engine_abi_set_compute_backend(cpu_name.as_ptr()), 1, "\"cpu\" siempre debe aceptarse");

        // --- Manejo de errores (PLAN.md §5.5): nunca panic/abort al otro lado de la ABI, un
        // nombre desconocido devuelve NULL + deja el detalle en engine_abi_last_error(). ----
        let unknown_name = c_string("NoExiste");
        let unknown = engine_abi_create_model(unknown_name.as_ptr(), std::ptr::null(), 0);
        assert!(unknown.is_null(), "se esperaba NULL al pedir un modelo inexistente");
        println!("error esperado al pedir un modelo inexistente: {}", last_error());

        println!("OK: ejemplo de Rust sobre engine/abi.h completado.");
    }
}

fn c_string(s: &str) -> CString {
    CString::new(s).expect("clave sin bytes NUL")
}
