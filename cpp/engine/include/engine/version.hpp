#pragma once

// Version de la distribucion inyectada por CMake. Mantener un valor por defecto permite
// incluir este header en builds directos fuera del proyecto principal.
#ifndef ENGINE_QUANT_VERSION
#define ENGINE_QUANT_VERSION "0.0.0-dev"
#endif

namespace engine {

inline constexpr const char* VERSION = ENGINE_QUANT_VERSION;

} // namespace engine
