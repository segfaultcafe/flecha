#include "runtime/runtime_abi.h"

#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {

void flecha_runtime_v1_stdout_string_view(const char* data, size_t length) {
    if (length == 0) return;
    (void)std::fwrite(data, 1, length, stdout);
}

void flecha_runtime_v1_stdout_i8(int8_t value) {
    (void)std::fprintf(stdout, "%" PRId8, value);
}

void flecha_runtime_v1_stdout_i16(int16_t value) {
    (void)std::fprintf(stdout, "%" PRId16, value);
}

void flecha_runtime_v1_stdout_i32(int32_t value) {
    (void)std::fprintf(stdout, "%" PRId32, value);
}

void flecha_runtime_v1_stdout_i64(int64_t value) {
    (void)std::fprintf(stdout, "%" PRId64, value);
}

void flecha_runtime_v1_stdout_isize(intptr_t value) {
    (void)std::fprintf(stdout, "%" PRIdPTR, value);
}

void flecha_runtime_v1_stdout_u8(uint8_t value) {
    (void)std::fprintf(stdout, "%" PRIu8, value);
}

void flecha_runtime_v1_stdout_u16(uint16_t value) {
    (void)std::fprintf(stdout, "%" PRIu16, value);
}

void flecha_runtime_v1_stdout_u32(uint32_t value) {
    (void)std::fprintf(stdout, "%" PRIu32, value);
}

void flecha_runtime_v1_stdout_u64(uint64_t value) {
    (void)std::fprintf(stdout, "%" PRIu64, value);
}

void flecha_runtime_v1_stdout_usize(uintptr_t value) {
    (void)std::fprintf(stdout, "%" PRIuPTR, value);
}

void flecha_runtime_v1_stdout_f32(float value) {
    (void)std::fprintf(stdout, "%.9g", static_cast<double>(value));
}

void flecha_runtime_v1_stdout_f64(double value) {
    (void)std::fprintf(stdout, "%.17g", value);
}

void flecha_runtime_v1_stdout_bool(uint8_t value) {
    const char* text = value == 0 ? "false" : "true";
    const size_t length = value == 0 ? 5 : 4;
    (void)std::fwrite(text, 1, length, stdout);
}

void flecha_runtime_v1_stdout_char(uint8_t value) {
    (void)std::fputc(value, stdout);
}

void flecha_runtime_v1_stdout_newline(void) {
    (void)std::fputc('\n', stdout);
}

uint8_t flecha_runtime_v1_string_view_at(const char* data, size_t length,
                                         uint32_t index) {
    if (index >= length) {
        (void)std::fprintf(
            stderr,
            "Flecha runtime error: stringv index %" PRIu32
            " is out of bounds for length %zu\n",
            index, length);
        std::abort();
    }
    return static_cast<uint8_t>(data[index]);
}

void flecha_runtime_v1_string_view_check_substr(size_t length, uint32_t start,
                                                uint32_t count) {
    if (start > length || count > length - start) {
        (void)std::fprintf(
            stderr,
            "Flecha runtime error: stringv substring (%" PRIu32 ", %" PRIu32
            ") is out of bounds for length %zu\n",
            start, count, length);
        std::abort();
    }
}

int32_t flecha_runtime_v1_string_try_from_bytes(FlechaRuntimeString* output,
                                                const uint8_t* data,
                                                size_t length) {
    *output = {nullptr, 0, 0};
    if (length == 0) return FLECHA_RUNTIME_STRING_OK;
    if (length == SIZE_MAX) return FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;

    char* owned = static_cast<char*>(std::malloc(length + 1));
    if (owned == nullptr) return FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;

    std::memcpy(owned, data, length);
    owned[length] = '\0';
    *output = {owned, length, length};
    return FLECHA_RUNTIME_STRING_OK;
}

void flecha_runtime_v1_string_from_view(FlechaRuntimeString* output,
                                        const char* data, size_t length) {
    if (length == 0) {
        *output = {nullptr, 0, 0};
        return;
    }
    if (length == SIZE_MAX) {
        (void)std::fputs(
            "Flecha runtime error: string allocation size overflow\n", stderr);
        std::abort();
    }
    char* owned = static_cast<char*>(std::malloc(length + 1));
    if (owned == nullptr) {
        (void)std::fputs("Flecha runtime error: string allocation failed\n",
                         stderr);
        std::abort();
    }
    std::memcpy(owned, data, length);
    owned[length] = '\0';
    *output = {owned, length, length};
}

void flecha_runtime_v1_string_clone(FlechaRuntimeString* output,
                                    const char* data, size_t length) {
    flecha_runtime_v1_string_from_view(output, data, length);
}

void flecha_runtime_v1_string_concat(FlechaRuntimeString* output,
                                     const char* leftData, size_t leftLength,
                                     const char* rightData,
                                     size_t rightLength) {
    if (rightLength > SIZE_MAX - leftLength) {
        (void)std::fputs(
            "Flecha runtime error: string concatenation size overflow\n",
            stderr);
        std::abort();
    }
    const size_t length = leftLength + rightLength;
    if (length == 0) {
        *output = {nullptr, 0, 0};
        return;
    }
    if (length == SIZE_MAX) {
        (void)std::fputs(
            "Flecha runtime error: string allocation size overflow\n", stderr);
        std::abort();
    }
    char* data = static_cast<char*>(std::malloc(length + 1));
    if (data == nullptr) {
        (void)std::fputs("Flecha runtime error: string allocation failed\n",
                         stderr);
        std::abort();
    }
    std::memcpy(data, leftData, leftLength);
    std::memcpy(data + leftLength, rightData, rightLength);
    data[length] = '\0';
    *output = {data, length, length};
}

void flecha_runtime_v1_string_destroy(char* data) {
    std::free(data);
}

#define FLECHA_FORMAT_STRING_FUNCTION(name, type, format)                    \
    void flecha_runtime_v1_string_from_##name(                               \
        FlechaRuntimeString* output, type value) {                           \
        char buffer[128];                                                     \
        const int length = std::snprintf(buffer, sizeof(buffer), format,      \
                                         value);                              \
        if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {    \
            (void)std::fputs(                                                 \
                "Flecha runtime error: scalar formatting failed\n", stderr); \
            std::abort();                                                     \
        }                                                                     \
        flecha_runtime_v1_string_from_view(                                  \
            output, buffer, static_cast<size_t>(length));                    \
    }

FLECHA_FORMAT_STRING_FUNCTION(i8, int8_t, "%" PRId8)
FLECHA_FORMAT_STRING_FUNCTION(i16, int16_t, "%" PRId16)
FLECHA_FORMAT_STRING_FUNCTION(i32, int32_t, "%" PRId32)
FLECHA_FORMAT_STRING_FUNCTION(i64, int64_t, "%" PRId64)
FLECHA_FORMAT_STRING_FUNCTION(isize, intptr_t, "%" PRIdPTR)
FLECHA_FORMAT_STRING_FUNCTION(u8, uint8_t, "%" PRIu8)
FLECHA_FORMAT_STRING_FUNCTION(u16, uint16_t, "%" PRIu16)
FLECHA_FORMAT_STRING_FUNCTION(u32, uint32_t, "%" PRIu32)
FLECHA_FORMAT_STRING_FUNCTION(u64, uint64_t, "%" PRIu64)
FLECHA_FORMAT_STRING_FUNCTION(usize, uintptr_t, "%" PRIuPTR)

void flecha_runtime_v1_string_from_f32(FlechaRuntimeString* output,
                                       float value) {
    char buffer[128];
    const int length = std::snprintf(buffer, sizeof(buffer), "%.9g",
                                     static_cast<double>(value));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
        (void)std::fputs(
            "Flecha runtime error: scalar formatting failed\n", stderr);
        std::abort();
    }
    flecha_runtime_v1_string_from_view(output, buffer,
                                       static_cast<size_t>(length));
}

void flecha_runtime_v1_string_from_f64(FlechaRuntimeString* output,
                                       double value) {
    char buffer[128];
    const int length =
        std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
        (void)std::fputs(
            "Flecha runtime error: scalar formatting failed\n", stderr);
        std::abort();
    }
    flecha_runtime_v1_string_from_view(output, buffer,
                                       static_cast<size_t>(length));
}

void flecha_runtime_v1_string_from_bool(FlechaRuntimeString* output,
                                        uint8_t value) {
    const char* text = value == 0 ? "false" : "true";
    flecha_runtime_v1_string_from_view(output, text, value == 0 ? 5 : 4);
}

void flecha_runtime_v1_string_from_char(FlechaRuntimeString* output,
                                        uint8_t value) {
    const char character = static_cast<char>(value);
    flecha_runtime_v1_string_from_view(output, &character, 1);
}

#undef FLECHA_FORMAT_STRING_FUNCTION

}  // extern "C"
