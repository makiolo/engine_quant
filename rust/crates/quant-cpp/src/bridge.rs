//! Versioned, flat C++ provider bridge.  Only this module names the C++ shim; legacy headers
//! are intentionally invisible to Rust and to the public adapter API.

#[cxx::bridge(namespace = "quant::bridge")]
pub mod ffi {
    #[repr(u8)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum CppErrorCode {
        Ok = 0,
        InvalidArgument = 1,
        Unsupported = 2,
        NumericalFailure = 3,
        ResourceExhausted = 4,
        Internal = 255,
    }

    struct CppStatus {
        code: CppErrorCode,
        message: String,
    }

    /// Version 1 shape. `times` is the number of schedule points and all arrays are contiguous
    /// row-major buffers. `scenarios` and `factors` are explicit even though the IRS leaf only
    /// supports one of each today, so a future bridge can add them without changing the ABI.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    struct BatchShape {
        trades: u64,
        scenarios: u64,
        factors: u64,
        times: u64,
    }

    unsafe extern "C++" {
        include!("quant-cpp/include/quant_cpp_bridge.hpp");

        type CppKernel;

        fn bridge_version() -> u32;
        fn new_kernel(kind: u32, config: &[u8]) -> Result<UniquePtr<CppKernel>>;
        fn capabilities(self: &CppKernel) -> u64;
        #[allow(clippy::too_many_arguments)]
        fn price_batch(
            self: &CppKernel,
            shape: BatchShape,
            market: &[f64],
            model: &[f64],
            products: &[f64],
            payment_times: &[f64],
            accruals: &[f64],
            output: &mut [f64],
        ) -> CppStatus;
    }
}
