#include "runtime/runtime_abi.h"

#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>

namespace {

bool TextEquals(const char* data, size_t length, const char* expected,
                size_t expectedLength) {
    return length == expectedLength &&
           std::memcmp(data, expected, expectedLength) == 0;
}

bool ValidDecimal(const char* first, const char* last) {
    if (first != last && (*first == '+' || *first == '-')) ++first;

    bool digits = false;
    while (first != last && *first >= '0' && *first <= '9') {
        digits = true;
        ++first;
    }
    if (first != last && *first == '.') {
        ++first;
        while (first != last && *first >= '0' && *first <= '9') {
            digits = true;
            ++first;
        }
    }
    if (!digits) return false;

    if (first != last && (*first == 'e' || *first == 'E')) {
        ++first;
        if (first != last && (*first == '+' || *first == '-')) ++first;

        const char* exponent = first;
        while (first != last && *first >= '0' && *first <= '9') ++first;
        if (first == exponent) return false;
    }

    return first == last;
}

template <typename T>
int32_t ParseFloat(T* output, const char* data, size_t length) {
    *output = 0;
    if (length == 0 || data == nullptr)
        return FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX;

    if (TextEquals(data, length, "nan", 3) ||
        TextEquals(data, length, "+nan", 4) ||
        TextEquals(data, length, "-nan", 4)) {
        *output = std::numeric_limits<T>::quiet_NaN();
        return FLECHA_RUNTIME_FLOAT_TEXT_OK;
    }
    if (TextEquals(data, length, "inf", 3) ||
        TextEquals(data, length, "+inf", 4)) {
        *output = std::numeric_limits<T>::infinity();
        return FLECHA_RUNTIME_FLOAT_TEXT_OK;
    }
    if (TextEquals(data, length, "-inf", 4)) {
        *output = -std::numeric_limits<T>::infinity();
        return FLECHA_RUNTIME_FLOAT_TEXT_OK;
    }

    const char* first = data;
    const char* last = data + length;
    if (!ValidDecimal(first, last))
        return FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX;
    if (*first == '+') {
        ++first;
    }

    T parsed = 0;
    const auto result =
        std::from_chars(first, last, parsed, std::chars_format::general);
    if (result.ec == std::errc::result_out_of_range)
        return FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_RANGE;
    if (result.ec == std::errc::invalid_argument || result.ptr != last)
        return FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX;

    *output = parsed;
    return FLECHA_RUNTIME_FLOAT_TEXT_OK;
}

template <typename T>
int32_t FormatFloat(uint8_t* destination, size_t capacity, T value,
                    size_t* written) {
    *written = 0;

    char buffer[128];
    size_t length = 0;
    if (std::isnan(value)) {
        std::memcpy(buffer, "nan", 3);
        length = 3;
    } else if (std::isinf(value)) {
        if (std::signbit(value)) {
            std::memcpy(buffer, "-inf", 4);
            length = 4;
        } else {
            std::memcpy(buffer, "inf", 3);
            length = 3;
        }
    } else {
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer),
                                          value, std::chars_format::general);
        if (result.ec != std::errc{})
            return FLECHA_RUNTIME_FLOAT_TEXT_INSUFFICIENT_SPACE;
        length = static_cast<size_t>(result.ptr - buffer);
    }

    if (capacity < length || (length != 0 && destination == nullptr))
        return FLECHA_RUNTIME_FLOAT_TEXT_INSUFFICIENT_SPACE;

    if (length != 0) std::memcpy(destination, buffer, length);
    *written = length;
    return FLECHA_RUNTIME_FLOAT_TEXT_OK;
}

template <typename T>
void PrintFloat(T value) {
    uint8_t buffer[128];
    size_t length = 0;
    if (FormatFloat(buffer, sizeof(buffer), value, &length) !=
        FLECHA_RUNTIME_FLOAT_TEXT_OK) {
        (void)std::fputs("Flecha runtime error: scalar formatting failed\n",
                         stderr);
        std::abort();
    }
    if (length != 0) (void)std::fwrite(buffer, 1, length, stdout);
}

template <typename T>
void StringFromFloat(FlechaRuntimeString* output, T value) {
    uint8_t buffer[128];
    size_t length = 0;
    if (FormatFloat(buffer, sizeof(buffer), value, &length) !=
        FLECHA_RUNTIME_FLOAT_TEXT_OK) {
        (void)std::fputs("Flecha runtime error: scalar formatting failed\n",
                         stderr);
        std::abort();
    }
    flecha_runtime_v1_string_from_view(
        output, reinterpret_cast<const char*>(buffer), length);
}

}  // namespace

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
    PrintFloat(value);
}

void flecha_runtime_v1_stdout_f64(double value) {
    PrintFloat(value);
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
    StringFromFloat(output, value);
}

void flecha_runtime_v1_string_from_f64(FlechaRuntimeString* output,
                                       double value) {
    StringFromFloat(output, value);
}

int32_t flecha_runtime_v1_parse_f32(float* output, const char* data,
                                    size_t length) {
    return ParseFloat(output, data, length);
}

int32_t flecha_runtime_v1_parse_f64(double* output, const char* data,
                                    size_t length) {
    return ParseFloat(output, data, length);
}

int32_t flecha_runtime_v1_format_f32(uint8_t* destination, size_t capacity,
                                     float value, size_t* written) {
    return FormatFloat(destination, capacity, value, written);
}

int32_t flecha_runtime_v1_format_f64(uint8_t* destination, size_t capacity,
                                     double value, size_t* written) {
    return FormatFloat(destination, capacity, value, written);
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
