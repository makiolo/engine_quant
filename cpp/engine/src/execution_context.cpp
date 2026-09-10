#include "engine/execution_context.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "engine/engine.hpp" // is_gpu_backend_available

namespace engine {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

ExecutionContext::ExecutionContext(const Params& params) {
    std::string requested_backend = to_lower(get_string(params, "backend"));
    if (requested_backend == "auto") {
        backend_ = is_gpu_backend_available() ? "gpu" : "cpu";
    } else if (requested_backend == "cpu" || requested_backend == "gpu") {
        backend_ = requested_backend;
    } else {
        throw std::invalid_argument(
            "ExecutionContext: backend desconocido: '" + requested_backend +
            "' (valores validos: \"cpu\", \"gpu\", \"auto\")");
    }

    std::string requested_precision = to_lower(get_string(params, "precision", "fp64"));
    if (requested_precision != "fp64") {
        throw std::invalid_argument(
            "ExecutionContext: precision no soportada: '" + requested_precision +
            "' (este build solo soporta \"FP64\", ver PLAN.md §5.1)");
    }
    precision_ = "FP64";
}

} // namespace engine
