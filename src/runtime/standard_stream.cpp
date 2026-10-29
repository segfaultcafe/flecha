#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>
#include <type_traits>

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

template <typename T>
int32_t ParseInteger(T* output, const char* data, size_t length) {
    *output = 0;
    if (data == nullptr || length == 0)
        return FLECHA_RUNTIME_INPUT_INVALID_SYNTAX;

    const char* first = data;
    const char* last = data + length;
    if (*first == '+') {
        ++first;
        if (first == last) return FLECHA_RUNTIME_INPUT_INVALID_SYNTAX;
    }
    T parsed = 0;
    const auto [end, error] = std::from_chars(first, last, parsed, 10);
    if (error == std::errc::result_out_of_range)
        return FLECHA_RUNTIME_INPUT_OUT_OF_RANGE;
    if (error != std::errc{} || end != last)
        return FLECHA_RUNTIME_INPUT_INVALID_SYNTAX;
    *output = parsed;
    return FLECHA_RUNTIME_INPUT_OK;
}

int32_t MapFloatInputStatus(int32_t status) {
    switch (status) {
        case FLECHA_RUNTIME_FLOAT_TEXT_OK:
            return FLECHA_RUNTIME_INPUT_OK;
        case FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_RANGE:
            return FLECHA_RUNTIME_INPUT_OUT_OF_RANGE;
        case FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_MEMORY:
            return FLECHA_RUNTIME_INPUT_OUT_OF_MEMORY;
        default:
            return FLECHA_RUNTIME_INPUT_INVALID_SYNTAX;
    }
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

int32_t flecha_runtime_v1_standard_output_flush(void) { return Flush(stdout); }

int32_t flecha_runtime_v1_standard_error_flush(void) { return Flush(stderr); }

int32_t flecha_runtime_v1_standard_input_read_line(
    FlechaRuntimeString* output) {
    *output = {nullptr, 0, 0};
    char* data = nullptr;
    size_t length = 0;
    size_t capacity = 0;

    std::clearerr(stdin);
    for (;;) {
        errno = 0;
        const int value = std::fgetc(stdin);
        if (value == EOF) {
            if (std::ferror(stdin)) {
                std::free(data);
                return FLECHA_RUNTIME_INPUT_READ_FAILED;
            }
            if (length == 0) {
                std::free(data);
                return FLECHA_RUNTIME_INPUT_END_OF_FILE;
            }
            break;
        }
        if (value == '\n') break;

        if (length == SIZE_MAX - 1) {
            std::free(data);
            return FLECHA_RUNTIME_INPUT_OUT_OF_MEMORY;
        }
        if (length + 1 >= capacity) {
            size_t next = capacity == 0 ? 64 : capacity;
            if (next > SIZE_MAX / 2)
                next = SIZE_MAX;
            else
                next *= 2;
            char* grown = static_cast<char*>(std::realloc(data, next));
            if (grown == nullptr) {
                std::free(data);
                return FLECHA_RUNTIME_INPUT_OUT_OF_MEMORY;
            }
            data = grown;
            capacity = next;
        }
        data[length++] = static_cast<char>(value);
    }

    if (length != 0 && data[length - 1] == '\r') --length;
    if (length == 0) {
        std::free(data);
        return FLECHA_RUNTIME_INPUT_OK;
    }
    data[length] = '\0';
    *output = {data, length, capacity - 1};
    return FLECHA_RUNTIME_INPUT_OK;
}

#define FLECHA_INPUT_PARSE_INTEGER(name, type)           \
    int32_t flecha_runtime_v1_input_parse_##name(        \
        type* output, const char* data, size_t length) { \
        return ParseInteger(output, data, length);       \
    }

FLECHA_INPUT_PARSE_INTEGER(i8, int8_t)
FLECHA_INPUT_PARSE_INTEGER(i16, int16_t)
FLECHA_INPUT_PARSE_INTEGER(i32, int32_t)
FLECHA_INPUT_PARSE_INTEGER(i64, int64_t)
FLECHA_INPUT_PARSE_INTEGER(isize, intptr_t)
FLECHA_INPUT_PARSE_INTEGER(u8, uint8_t)
FLECHA_INPUT_PARSE_INTEGER(u16, uint16_t)
FLECHA_INPUT_PARSE_INTEGER(u32, uint32_t)
FLECHA_INPUT_PARSE_INTEGER(u64, uint64_t)
FLECHA_INPUT_PARSE_INTEGER(usize, uintptr_t)

#undef FLECHA_INPUT_PARSE_INTEGER

int32_t flecha_runtime_v1_input_parse_f32(float* output, const char* data,
                                          size_t length) {
    return MapFloatInputStatus(
        flecha_runtime_v1_parse_f32(output, data, length));
}

int32_t flecha_runtime_v1_input_parse_f64(double* output, const char* data,
                                          size_t length) {
    return MapFloatInputStatus(
        flecha_runtime_v1_parse_f64(output, data, length));
}

int32_t flecha_runtime_v1_input_parse_bool(uint8_t* output, const char* data,
                                           size_t length) {
    *output = 0;
    if (data != nullptr && length == 4 && std::memcmp(data, "true", 4) == 0) {
        *output = 1;
        return FLECHA_RUNTIME_INPUT_OK;
    }
    if (data != nullptr && length == 5 && std::memcmp(data, "false", 5) == 0)
        return FLECHA_RUNTIME_INPUT_OK;
    return FLECHA_RUNTIME_INPUT_INVALID_SYNTAX;
}

int32_t flecha_runtime_v1_input_parse_char(uint8_t* output, const char* data,
                                           size_t length) {
    *output = 0;
    if (data == nullptr || length != 1)
        return FLECHA_RUNTIME_INPUT_INVALID_SYNTAX;
    *output = static_cast<uint8_t>(data[0]);
    return FLECHA_RUNTIME_INPUT_OK;
}

}  // extern "C"
