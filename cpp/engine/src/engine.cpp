#include "engine/engine.hpp"

#include "engine-ffi-cxx/lib.h"

namespace engine {

double ping() {
    return ffi::ping();
}

} // namespace engine
