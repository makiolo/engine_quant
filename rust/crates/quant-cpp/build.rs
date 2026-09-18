fn main() {
    cxx_build::bridge("src/bridge.rs")
        .file("../../../cpp/quant-cpp/src/quant_cpp_bridge.cpp")
        .include("../../../cpp")
        .std("c++17")
        .compile("quant-cpp-bridge");

    println!("cargo:rerun-if-changed=src/bridge.rs");
    println!("cargo:rerun-if-changed=../../../cpp/quant-cpp/include/quant_cpp_bridge.hpp");
    println!("cargo:rerun-if-changed=../../../cpp/quant-cpp/include/legacy_irs_leaf.hpp");
    println!("cargo:rerun-if-changed=../../../cpp/quant-cpp/src/quant_cpp_bridge.cpp");
}
