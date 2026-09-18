#pragma once

#include <cstdint>
#include <memory>

// cxx needs the opaque type forward-declared before its generated header aliases it.
namespace quant::bridge {
class CppKernel;
}

// The generated cxx header is the only Rust-facing dependency. Legacy C++ headers are included
// by the shim implementation, never by Rust or by downstream users of quant-cpp.
#include "quant-cpp/src/bridge.rs.h"

namespace quant::bridge {

class CppKernel {
public:
    explicit CppKernel(std::uint32_t kind) noexcept : kind_(kind) {}

    std::uint64_t capabilities() const noexcept;
    CppStatus price_batch(
        BatchShape shape,
        rust::Slice<const double> market,
        rust::Slice<const double> model,
        rust::Slice<const double> products,
        rust::Slice<const double> payment_times,
        rust::Slice<const double> accruals,
        rust::Slice<double> output) const;

private:
    std::uint32_t kind_;

    friend std::unique_ptr<CppKernel> new_kernel(std::uint32_t, rust::Slice<const std::uint8_t>);
};

std::uint32_t bridge_version() noexcept;
std::unique_ptr<CppKernel> new_kernel(std::uint32_t kind, rust::Slice<const std::uint8_t> config);
}  // namespace quant::bridge
