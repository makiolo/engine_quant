//! Ejemplo de Rust consumiendo `engine/abi.h` (PLAN.md Fase 6, §5.5/§7.13; ENGINE.CALC en
//! PLAN.md §7.15) vía FFI directa a C, SIN pasar por los crates internos
//! `engine-core`/`engine-ffi` (esos hablan con la capa C++ vía `cxx`, un mecanismo distinto e
//! interno) -- deliberado: demuestra que incluso Rust, que ya tiene acceso privilegiado al
//! motor, podría consumirlo igual que Julia/.NET/Go si hiciera falta (ej. un proceso Rust
//! separado que solo tiene el `.dll`/`.so` compilado, no este repo). Las declaraciones
//! `extern "C"` de abajo son la traducción manual a Rust de `cpp/engine/include/engine/abi.h`
//! -- lo que generaría `bindgen` automáticamente.
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
struct EngineMarketSnapshot {
    pillars: *const f64,
    zero_rates: *const f64,
    count: usize,
    hazard_rate: f64,
    recovery_rate: f64,
}

#[repr(C)]
#[derive(Default)]
struct EnginePricingContext {
    pricing_date: f64,
    n_paths: u64,
    n_steps: u64,
    seed: u64,
}

#[repr(C)]
struct EngineExecutionContext {
    backend: *const c_char,
    precision: *const c_char,
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

#[repr(C)]
struct EngineCalcResultEntry {
    measure_name: *mut c_char,
    result: EngineMeasureResult,
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
    fn engine_abi_free_model(model: *mut EngineModel);
    fn engine_abi_free_product(product: *mut EngineProduct);

    fn engine_abi_calc(
        product: *const EngineProduct,
        measure_names: *const *const c_char,
        n_measure_names: usize,
        model: *const EngineModel,
        market: *const EngineMarketSnapshot,
        pricing: *const EnginePricingContext,
        execution: *const EngineExecutionContext,
        out_entries: *mut *mut EngineCalcResultEntry,
        out_count: *mut usize,
    ) -> c_int;
    fn engine_abi_free_calc_results(entries: *mut EngineCalcResultEntry, count: usize);

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

// Busca una medida por nombre en el array crudo que devuelve engine_abi_calc -- lifetime
// ligado a `entries`, liberado explícitamente por el llamador con engine_abi_free_calc_results.
unsafe fn find_measure<'a>(entries: &'a [EngineCalcResultEntry], name: &str) -> &'a EngineMeasureResult {
    for entry in entries {
        if CStr::from_ptr(entry.measure_name).to_str() == Ok(name) {
            return &entry.result;
        }
    }
    panic!("no se pidio la medida '{name}'");
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

        // --- ENGINE.CALC (PLAN.md §7.15): mismo caso base que cpp/engine/tests/
        // test_registry.cpp (Registry.UnilateralCvaMatchesGoldenValue/
        // ExposureProfileMatchesGoldenValue), pero aquí basta con invariantes cualitativos --
        // esta sonda verifica el mecanismo de la ABI, no vuelve a fijar el numero exacto. -----
        let pillars = [1.0_f64];
        let zero_rates = [0.02_f64];
        let market = EngineMarketSnapshot {
            pillars: pillars.as_ptr(),
            zero_rates: zero_rates.as_ptr(),
            count: 1,
            hazard_rate: 0.02,
            recovery_rate: 0.4,
        };
        let pricing = EnginePricingContext { pricing_date: 0.0, n_paths: 5000, n_steps: 208, seed: 7 };
        let (backend_cpu, precision_fp64) = (c_string("cpu"), c_string("FP64"));
        let execution = EngineExecutionContext { backend: backend_cpu.as_ptr(), precision: precision_fp64.as_ptr() };

        let measure_names = [c_string("PV"), c_string("DV01"), c_string("ExpectedExposure"), c_string("PFE95"), c_string("UnilateralCVA")];
        let measure_name_ptrs: Vec<*const c_char> = measure_names.iter().map(|s| s.as_ptr()).collect();

        let mut entries_ptr: *mut EngineCalcResultEntry = std::ptr::null_mut();
        let mut entry_count: usize = 0;
        let rc = engine_abi_calc(
            product,
            measure_name_ptrs.as_ptr(),
            measure_name_ptrs.len(),
            model,
            &market,
            &pricing,
            &execution,
            &mut entries_ptr,
            &mut entry_count,
        );
        assert_eq!(rc, 0, "engine_abi_calc: {}", last_error());
        let entries = std::slice::from_raw_parts(entries_ptr, entry_count);

        let pv = find_measure(entries, "PV");
        let dv01 = find_measure(entries, "DV01");
        let ee = find_measure(entries, "ExpectedExposure");
        let pfe = find_measure(entries, "PFE95");
        let cva = find_measure(entries, "UnilateralCVA");

        println!("PV            = {}  (swap a la par: ~0)", pv.scalar);
        println!("DV01          = {}  (swap pagador: > 0)", dv01.scalar);
        println!("UnilateralCVA = {}  (> 0 con hazard_rate > 0)", cva.scalar);
        let ee_times = std::slice::from_raw_parts(ee.times, ee.len);
        let ee_values = std::slice::from_raw_parts(ee.primary, ee.len);
        let pfe_values = std::slice::from_raw_parts(pfe.primary, pfe.len);
        for i in 0..ee.len {
            println!("  t={}: EE={}  PFE95={}", ee_times[i], ee_values[i], pfe_values[i]);
        }

        assert!(pv.scalar.abs() < 1e-6, "PV de un swap a la par deberia ser ~0");
        assert!(dv01.scalar > 0.0 && cva.scalar > 0.0, "se esperaba DV01 > 0 y UnilateralCVA > 0");
        for i in 0..ee.len {
            assert!(ee_values[i] >= 0.0 && pfe_values[i] >= ee_values[i], "se esperaba ExpectedExposure >= 0 y PFE95 >= ExpectedExposure");
        }

        engine_abi_free_calc_results(entries_ptr, entry_count);

        println!("gpu disponible: {}", if engine_abi_is_gpu_backend_available() != 0 { "si" } else { "no" });

        // engine_abi_calc rechaza un nombre de medida desconocido (PLAN.md §7.15): el error
        // queda en engine_abi_last_error(), nunca panic/abort a traves de esta frontera C.
        let bad_name = c_string("NoExiste");
        let bad_name_ptrs = [bad_name.as_ptr()];
        let mut bad_entries: *mut EngineCalcResultEntry = std::ptr::null_mut();
        let mut bad_count: usize = 0;
        let rc = engine_abi_calc(
            product, bad_name_ptrs.as_ptr(), 1, model, &market, &pricing, &execution, &mut bad_entries, &mut bad_count,
        );
        assert_ne!(rc, 0, "se esperaba error con un nombre de medida desconocido");
        println!("error esperado al pedir una medida inexistente: {}", last_error());

        engine_abi_free_product(product);
        engine_abi_free_model(model);

        // --- Manejo de errores (PLAN.md §5.5): nunca panic/abort al otro lado de la ABI, un
        // nombre desconocido devuelve NULL + deja el detalle en engine_abi_last_error(). ----
        let unknown_name = c_string("NoExiste");
        let unknown = engine_abi_create_model(unknown_name.as_ptr(), std::ptr::null(), 0);
        assert!(unknown.is_null(), "se esperaba NULL al pedir un modelo inexistente");
        println!("error esperado al pedir un modelo inexistente: {}", last_error());

        println!("OK: ejemplo de Rust sobre engine/abi.h (ENGINE.CALC) completado.");
    }
}

fn c_string(s: &str) -> CString {
    CString::new(s).expect("clave sin bytes NUL")
}
