#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
// BCrypt declarations depend on base Win32 types. Keep this order even when
// formatting the translation unit.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#elif defined(__APPLE__)
#include <unistd.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <sys/types.h>
#else
#error "Flecha secure randomness is unsupported on this platform"
#endif

namespace {

#if defined(__APPLE__) || defined(__linux__)
int32_t MapRandomError(int error) {
#ifdef ENOSYS
    if (error == ENOSYS) return FLECHA_RUNTIME_RANDOM_UNAVAILABLE;
#else
    (void)error;
#endif
    return FLECHA_RUNTIME_RANDOM_FAILED;
}
#endif

#ifdef _WIN32

int32_t FillRandom(uint8_t* destination, size_t length) {
    constexpr size_t MAXIMUM_REQUEST =
        static_cast<size_t>(std::numeric_limits<ULONG>::max());

    while (length != 0) {
        const size_t chunk = std::min(length, MAXIMUM_REQUEST);

        const NTSTATUS status =
            BCryptGenRandom(nullptr, destination, static_cast<ULONG>(chunk),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status)) {
            return FLECHA_RUNTIME_RANDOM_FAILED;
        }

        destination += chunk;
        length -= chunk;
    }

    return FLECHA_RUNTIME_RANDOM_OK;
}

#elif defined(__APPLE__)

int32_t FillRandom(uint8_t* destination, size_t length) {
    constexpr size_t MAXIMUM_REQUEST = 256;

    while (length != 0) {
        const size_t chunk = std::min(length, MAXIMUM_REQUEST);

        if (getentropy(destination, chunk) != 0) {
            if (errno == EINTR) continue;
            return MapRandomError(errno);
        }

        destination += chunk;
        length -= chunk;
    }

    return FLECHA_RUNTIME_RANDOM_OK;
}

#elif defined(__linux__)

int32_t FillRandom(uint8_t* destination, size_t length) {
    constexpr size_t MAXIMUM_REQUEST =
        static_cast<size_t>(std::numeric_limits<ssize_t>::max());

    while (length != 0) {
        const size_t chunk = std::min(length, MAXIMUM_REQUEST);
        const ssize_t result = getrandom(destination, chunk, 0);

        if (result > 0) {
            const size_t completed = static_cast<size_t>(result);
            destination += completed;
            length -= completed;
            continue;
        }

        if (result == 0) {
            return FLECHA_RUNTIME_RANDOM_FAILED;
        }

        if (errno == EINTR) continue;
        return MapRandomError(errno);
    }

    return FLECHA_RUNTIME_RANDOM_OK;
}

#endif

}  // namespace

extern "C" {

int32_t flecha_runtime_v1_secure_random_fill(uint8_t* destination,
                                             size_t length) {
    if (length == 0) return FLECHA_RUNTIME_RANDOM_OK;
    if (destination == nullptr) return FLECHA_RUNTIME_RANDOM_FAILED;

    return FillRandom(destination, length);
}

}  // extern "C"
