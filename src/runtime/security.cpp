#include <cstddef>
#include <cstdint>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
// SecureZeroMemory is declared by the base Win32 headers.
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <string.h>
#else
#error "Flecha secure memory operations are unsupported on this platform"
#endif

uint8_t flecha_runtime_v1_constant_time_difference(const uint8_t* left,
                                                   const uint8_t* right,
                                                   size_t length) {
    if (length != 0 && (left == nullptr || right == nullptr)) {
        return UINT8_MAX;
    }

    // Volatile reads make every input byte observable to the implementation,
    // preventing an optimizer from replacing this loop with an early-exiting
    // comparison. The loop count depends only on the public length.
    const volatile uint8_t* observed_left = left;
    const volatile uint8_t* observed_right = right;
    uint8_t difference = 0;

    for (size_t index = 0; index < length; ++index) {
        difference = static_cast<uint8_t>(
            difference |
            static_cast<uint8_t>(observed_left[index] ^ observed_right[index]));
    }

    return difference;
}

void flecha_runtime_v1_secure_zero(uint8_t* destination, size_t length) {
    if (length == 0) return;

#ifdef _WIN32
    SecureZeroMemory(destination, length);
#else
    explicit_bzero(destination, length);
#endif
}
