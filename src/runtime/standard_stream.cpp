#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

int32_t MapStreamError(int error) {
    switch (error) {
        case EACCES:
#if defined(EPERM) && EPERM != EACCES
        case EPERM:
#endif
            return FLECHA_RUNTIME_IO_PERMISSION_DENIED;
#if defined(EFBIG)
        case EFBIG:
#endif
#if defined(EOVERFLOW) && EOVERFLOW != EFBIG
        case EOVERFLOW:
#endif
            return FLECHA_RUNTIME_IO_TOO_LARGE;
        default:
            return FLECHA_RUNTIME_IO_FAILED;
    }
}

int32_t Write(FILE* stream, const uint8_t* source, size_t length,
              size_t* bytesWritten) {
    if (bytesWritten == nullptr) return FLECHA_RUNTIME_IO_INVALID_OPTIONS;
    *bytesWritten = 0;
    if (source == nullptr && length != 0)
        return FLECHA_RUNTIME_IO_INVALID_OPTIONS;
    if (length == 0) return FLECHA_RUNTIME_IO_OK;

    // Preserve ordering with the typed output built-ins, which share the C
    // stream, before bypassing its user-space buffer for an exact byte write.
    std::clearerr(stream);
    errno = 0;
    if (std::fflush(stream) != 0) return MapStreamError(errno);

#ifdef _WIN32
    const int descriptor = _fileno(stream);
    if (descriptor < 0) return FLECHA_RUNTIME_IO_FAILED;
    const unsigned int amount = static_cast<unsigned int>(
        std::min(length, static_cast<size_t>(UINT_MAX)));
    errno = 0;
    const int result = _write(descriptor, source, amount);
#else
    const int descriptor = fileno(stream);
    if (descriptor < 0) return FLECHA_RUNTIME_IO_FAILED;
    const size_t amount = std::min(
        length, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    ssize_t result;
    do {
        errno = 0;
        result = ::write(descriptor, source, amount);
    } while (result < 0 && errno == EINTR);
#endif
    if (result < 0) return MapStreamError(errno);
    *bytesWritten = static_cast<size_t>(result);
    return FLECHA_RUNTIME_IO_OK;
}

int32_t Flush(FILE* stream) {
    std::clearerr(stream);
    errno = 0;
    return std::fflush(stream) == 0 ? FLECHA_RUNTIME_IO_OK
                                    : MapStreamError(errno);
}

}  // namespace

extern "C" {

int32_t flecha_runtime_v1_standard_output_write(const uint8_t* source,
                                                size_t length,
                                                size_t* bytes_written) {
    return Write(stdout, source, length, bytes_written);
}

int32_t flecha_runtime_v1_standard_error_write(const uint8_t* source,
                                               size_t length,
                                               size_t* bytes_written) {
    return Write(stderr, source, length, bytes_written);
}

int32_t flecha_runtime_v1_standard_output_flush(void) {
    return Flush(stdout);
}

int32_t flecha_runtime_v1_standard_error_flush(void) {
    return Flush(stderr);
}

}  // extern "C"
