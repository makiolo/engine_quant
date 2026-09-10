// Enlaza este ejemplo contra el `engine_abi` ya compilado por el CMake de la raíz del repo
// (PLAN.md §7.13) -- deliberadamente NO es un miembro de rust/Cargo.toml (ese workspace habla
// con la capa C++ vía cxx/engine-ffi, un mecanismo interno distinto de esta ABI pública) ni
// depende de engine-core/engine-ffi: solo ve engine_abi.dll/.lib, exactamente lo que vería
// cualquier otro consumidor externo con FFI a C.

use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let default_lib_dir = manifest_dir.join("../../../build/cpp/engine");
    let lib_dir = env::var_os("ENGINE_ABI_LIB_DIR")
        .map(PathBuf::from)
        .unwrap_or(default_lib_dir);

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=engine_abi");
    println!("cargo:rerun-if-env-changed=ENGINE_ABI_LIB_DIR");
}
