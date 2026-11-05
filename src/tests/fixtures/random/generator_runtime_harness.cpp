#include <cstddef>
#include <cstdint>

#include "runtime/runtime_abi.h"

extern "C" int32_t flecha_runtime_v1_secure_random_fill(uint8_t* destination,
                                                        std::size_t length) {
    if (length == 1) return FLECHA_RUNTIME_RANDOM_UNAVAILABLE;
    if (length == 2) return FLECHA_RUNTIME_RANDOM_FAILED;
    if (destination == nullptr || length != 32)
        return FLECHA_RUNTIME_RANDOM_FAILED;

    for (std::size_t index = 0; index < length; ++index)
        destination[index] = static_cast<uint8_t>(index);
    return FLECHA_RUNTIME_RANDOM_OK;
}
