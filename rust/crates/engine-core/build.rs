use std::fs;

fn main() {
    println!("cargo:rerun-if-changed=../../VERSION");
    let version = fs::read_to_string("../../VERSION")
        .expect("rust/VERSION debe existir para compilar engine-core");
    println!("cargo:rustc-env=ENGINE_QUANT_VERSION={}", version.trim());
}
