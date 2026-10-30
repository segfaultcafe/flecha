#include <cstddef>
#include <cstdint>

#include "runtime/runtime_abi.h"

extern "C" int32_t __wrap_flecha_runtime_v1_environment_get(
    FlechaRuntimeString* output, uint8_t* present, const char*, std::size_t) {
    *output = {nullptr, 0, 0};
    *present = 0;
    return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
}

extern "C" int32_t __wrap_flecha_runtime_v1_environment_set(const char*,
                                                            std::size_t,
                                                            const char*,
                                                            std::size_t) {
    return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
}

extern "C" int32_t __wrap_flecha_runtime_v1_environment_remove(const char*,
                                                               std::size_t) {
    return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
}
