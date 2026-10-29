#include <locale.h>

#include <bit>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

#include "runtime/runtime_abi.h"

#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace {

#ifdef _WIN32
using FlechaNumericLocale = _locale_t;
#else
using FlechaNumericLocale = locale_t;
#endif

class NumericLocale final {
   public:
    NumericLocale() {
#ifdef _WIN32
        _value = _create_locale(LC_NUMERIC, "C");
#else
        _value = newlocale(LC_NUMERIC_MASK, "C", nullptr);
#endif
        if (_value == nullptr) {
            (void)std::fputs(
                "Flecha runtime error: C numeric locale is unavailable\n",
                stderr);
            std::abort();
        }
    }

    ~NumericLocale() {
#ifdef _WIN32
        _free_locale(_value);
#else
        freelocale(_value);
#endif
    }

    NumericLocale(const NumericLocale&) = delete;
    NumericLocale& operator=(const NumericLocale&) = delete;

    FlechaNumericLocale Get() const { return _value; }

   private:
    FlechaNumericLocale _value = nullptr;
};

FlechaNumericLocale CLocale() {
    static const NumericLocale locale;
    return locale.Get();
}

template <typename T>
T ParseWithCLocale(const char* text, char** end) {
#ifdef _WIN32
    if constexpr (std::is_same_v<T, float>)
        return _strtof_l(text, end, CLocale());
    else
        return _strtod_l(text, end, CLocale());
#else
    const locale_t previous = uselocale(CLocale());
    T result;
    if constexpr (std::is_same_v<T, float>)
        result = std::strtof(text, end);
    else
        result = std::strtod(text, end);
    (void)uselocale(previous);
    return result;
#endif
}

template <typename T>
int FormatWithCLocale(char* destination, size_t capacity, int precision,
                      T value) {
#ifdef _WIN32
    return _snprintf_l(destination, capacity, "%.*g", CLocale(), precision,
                       static_cast<double>(value));
#else
    const locale_t previous = uselocale(CLocale());
    const int length = std::snprintf(destination, capacity, "%.*g", precision,
                                     static_cast<double>(value));
    (void)uselocale(previous);
    return length;
#endif
}

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
    if (*first == '+') ++first;

    const size_t finiteLength = static_cast<size_t>(last - first);
    char localText[128];
    char* text = localText;
    if (finiteLength >= sizeof(localText)) {
        if (finiteLength == SIZE_MAX)
            return FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_MEMORY;
        text = static_cast<char*>(std::malloc(finiteLength + 1));
        if (text == nullptr) return FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_MEMORY;
    }
    std::memcpy(text, first, finiteLength);
    text[finiteLength] = '\0';

    errno = 0;
    char* end = nullptr;
    const T parsed = ParseWithCLocale<T>(text, &end);
    const int parseError = errno;
    const bool complete = end == text + finiteLength;
    if (text != localText) std::free(text);

    if (!complete) return FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX;
    if (parseError == ERANGE && (parsed == 0 || std::isinf(parsed)))
        return FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_RANGE;

    *output = parsed;
    return FLECHA_RUNTIME_FLOAT_TEXT_OK;
}

template <typename T>
bool SameFloat(T left, T right) {
    if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
    else
        return std::bit_cast<uint64_t>(left) == std::bit_cast<uint64_t>(right);
}

template <typename T>
size_t FormatFinite(char* buffer, size_t capacity, T value) {
    constexpr int MAX_DIGITS = std::numeric_limits<T>::max_digits10;

    for (int precision = 1; precision <= MAX_DIGITS; ++precision) {
        const int candidateLength =
            FormatWithCLocale(buffer, capacity, precision, value);
        if (candidateLength < 0 ||
            static_cast<size_t>(candidateLength) >= capacity)
            return 0;

        errno = 0;
        char* end = nullptr;
        const T parsed = ParseWithCLocale<T>(buffer, &end);
        const bool representableRange =
            errno != ERANGE || (parsed != 0 && !std::isinf(parsed));
        if (representableRange && end == buffer + candidateLength &&
            SameFloat(parsed, value))
            return static_cast<size_t>(candidateLength);
    }

    return 0;
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
        length = FormatFinite(buffer, sizeof(buffer), value);
        if (length == 0) return FLECHA_RUNTIME_FLOAT_TEXT_INSUFFICIENT_SPACE;
    }

    if (capacity < length || (length != 0 && destination == nullptr))
        return FLECHA_RUNTIME_FLOAT_TEXT_INSUFFICIENT_SPACE;

    if (length != 0) std::memcpy(destination, buffer, length);
    *written = length;
    return FLECHA_RUNTIME_FLOAT_TEXT_OK;
}

template <typename T>
void PrintFloat(FILE* stream, T value) {
    uint8_t buffer[128];
    size_t length = 0;
    if (FormatFloat(buffer, sizeof(buffer), value, &length) !=
        FLECHA_RUNTIME_FLOAT_TEXT_OK) {
        (void)std::fputs("Flecha runtime error: scalar formatting failed\n",
                         stderr);
        std::abort();
    }
    if (length != 0) (void)std::fwrite(buffer, 1, length, stream);
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

void flecha_runtime_v1_stdout_f32(float value) { PrintFloat(stdout, value); }

void flecha_runtime_v1_stdout_f64(double value) { PrintFloat(stdout, value); }

void flecha_runtime_v1_stdout_bool(uint8_t value) {
    const char* text = value == 0 ? "false" : "true";
    const size_t length = value == 0 ? 5 : 4;
    (void)std::fwrite(text, 1, length, stdout);
}

void flecha_runtime_v1_stdout_char(uint8_t value) {
    (void)std::fputc(value, stdout);
}

void flecha_runtime_v1_stdout_newline(void) { (void)std::fputc('\n', stdout); }

void flecha_runtime_v1_stderr_string_view(const char* data, size_t length) {
    if (length == 0) return;
    (void)std::fwrite(data, 1, length, stderr);
}

void flecha_runtime_v1_stderr_i8(int8_t value) {
    (void)std::fprintf(stderr, "%" PRId8, value);
}

void flecha_runtime_v1_stderr_i16(int16_t value) {
    (void)std::fprintf(stderr, "%" PRId16, value);
}

void flecha_runtime_v1_stderr_i32(int32_t value) {
    (void)std::fprintf(stderr, "%" PRId32, value);
}

void flecha_runtime_v1_stderr_i64(int64_t value) {
    (void)std::fprintf(stderr, "%" PRId64, value);
}

void flecha_runtime_v1_stderr_isize(intptr_t value) {
    (void)std::fprintf(stderr, "%" PRIdPTR, value);
}

void flecha_runtime_v1_stderr_u8(uint8_t value) {
    (void)std::fprintf(stderr, "%" PRIu8, value);
}

void flecha_runtime_v1_stderr_u16(uint16_t value) {
    (void)std::fprintf(stderr, "%" PRIu16, value);
}

void flecha_runtime_v1_stderr_u32(uint32_t value) {
    (void)std::fprintf(stderr, "%" PRIu32, value);
}

void flecha_runtime_v1_stderr_u64(uint64_t value) {
    (void)std::fprintf(stderr, "%" PRIu64, value);
}

void flecha_runtime_v1_stderr_usize(uintptr_t value) {
    (void)std::fprintf(stderr, "%" PRIuPTR, value);
}

void flecha_runtime_v1_stderr_f32(float value) { PrintFloat(stderr, value); }

void flecha_runtime_v1_stderr_f64(double value) { PrintFloat(stderr, value); }

void flecha_runtime_v1_stderr_bool(uint8_t value) {
    const char* text = value == 0 ? "false" : "true";
    const size_t length = value == 0 ? 5 : 4;
    (void)std::fwrite(text, 1, length, stderr);
}

void flecha_runtime_v1_stderr_char(uint8_t value) {
    (void)std::fputc(value, stderr);
}

void flecha_runtime_v1_stderr_newline(void) { (void)std::fputc('\n', stderr); }

uint8_t flecha_runtime_v1_string_view_at(const char* data, size_t length,
                                         uint32_t index) {
    if (index >= length) {
        (void)std::fprintf(stderr,
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
        (void)std::fprintf(stderr,
                           "Flecha runtime error: stringv substring (%" PRIu32
                           ", %" PRIu32 ") is out of bounds for length %zu\n",
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

void flecha_runtime_v1_format_buffer_init(FlechaRuntimeFormatBuffer* buffer) {
    *buffer = {nullptr, 0, 0, FLECHA_RUNTIME_STRING_OK};
}

namespace {

bool FormatBufferReserve(FlechaRuntimeFormatBuffer* buffer, size_t additional) {
    if (buffer->status != FLECHA_RUNTIME_STRING_OK) return false;
    if (additional > SIZE_MAX - buffer->length - 1) {
        buffer->status = FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;
        return false;
    }
    const size_t required = buffer->length + additional + 1;
    if (required <= buffer->capacity) return true;

    size_t capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char* data = static_cast<char*>(std::realloc(buffer->data, capacity));
    if (data == nullptr) {
        buffer->status = FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

void FormatBufferAppend(FlechaRuntimeFormatBuffer* buffer, const char* data,
                        size_t length) {
    if (length == 0 || !FormatBufferReserve(buffer, length)) return;
    if (data == nullptr) {
        buffer->status = FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;
        return;
    }
    std::memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

}  // namespace

void flecha_runtime_v1_format_buffer_append_string_view(
    FlechaRuntimeFormatBuffer* buffer, const char* data, size_t length) {
    FormatBufferAppend(buffer, data, length);
}

#define FLECHA_FORMAT_BUFFER_APPEND_INTEGER(name, type, format)               \
    void flecha_runtime_v1_format_buffer_append_##name(                       \
        FlechaRuntimeFormatBuffer* buffer, type value) {                      \
        char formatted[128];                                                  \
        const int length =                                                    \
            std::snprintf(formatted, sizeof(formatted), format, value);       \
        if (length < 0 || static_cast<size_t>(length) >= sizeof(formatted)) { \
            buffer->status = FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;             \
            return;                                                           \
        }                                                                     \
        FormatBufferAppend(buffer, formatted, static_cast<size_t>(length));   \
    }

FLECHA_FORMAT_BUFFER_APPEND_INTEGER(i8, int8_t, "%" PRId8)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(i16, int16_t, "%" PRId16)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(i32, int32_t, "%" PRId32)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(i64, int64_t, "%" PRId64)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(isize, intptr_t, "%" PRIdPTR)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(u8, uint8_t, "%" PRIu8)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(u16, uint16_t, "%" PRIu16)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(u32, uint32_t, "%" PRIu32)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(u64, uint64_t, "%" PRIu64)
FLECHA_FORMAT_BUFFER_APPEND_INTEGER(usize, uintptr_t, "%" PRIuPTR)

#undef FLECHA_FORMAT_BUFFER_APPEND_INTEGER

void flecha_runtime_v1_format_buffer_append_f32(
    FlechaRuntimeFormatBuffer* buffer, float value) {
    uint8_t formatted[128];
    size_t length = 0;
    if (FormatFloat(formatted, sizeof(formatted), value, &length) !=
        FLECHA_RUNTIME_FLOAT_TEXT_OK) {
        buffer->status = FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;
        return;
    }
    FormatBufferAppend(buffer, reinterpret_cast<const char*>(formatted),
                       length);
}

void flecha_runtime_v1_format_buffer_append_f64(
    FlechaRuntimeFormatBuffer* buffer, double value) {
    uint8_t formatted[128];
    size_t length = 0;
    if (FormatFloat(formatted, sizeof(formatted), value, &length) !=
        FLECHA_RUNTIME_FLOAT_TEXT_OK) {
        buffer->status = FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;
        return;
    }
    FormatBufferAppend(buffer, reinterpret_cast<const char*>(formatted),
                       length);
}

void flecha_runtime_v1_format_buffer_append_bool(
    FlechaRuntimeFormatBuffer* buffer, uint8_t value) {
    const char* text = value == 0 ? "false" : "true";
    FormatBufferAppend(buffer, text, value == 0 ? 5 : 4);
}

void flecha_runtime_v1_format_buffer_append_char(
    FlechaRuntimeFormatBuffer* buffer, uint8_t value) {
    const char character = static_cast<char>(value);
    FormatBufferAppend(buffer, &character, 1);
}

int32_t flecha_runtime_v1_format_buffer_finish(
    FlechaRuntimeFormatBuffer* buffer, FlechaRuntimeString* output) {
    *output = {nullptr, 0, 0};
    if (buffer->status != FLECHA_RUNTIME_STRING_OK) {
        std::free(buffer->data);
        flecha_runtime_v1_format_buffer_init(buffer);
        return FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;
    }
    if (buffer->length == 0) {
        std::free(buffer->data);
        flecha_runtime_v1_format_buffer_init(buffer);
        return FLECHA_RUNTIME_STRING_OK;
    }

    *output = {buffer->data, buffer->length, buffer->capacity - 1};
    buffer->data = nullptr;
    flecha_runtime_v1_format_buffer_init(buffer);
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

void flecha_runtime_v1_string_destroy(char* data) { std::free(data); }

#define FLECHA_FORMAT_STRING_FUNCTION(name, type, format)                    \
    void flecha_runtime_v1_string_from_##name(FlechaRuntimeString* output,   \
                                              type value) {                  \
        char buffer[128];                                                    \
        const int length =                                                   \
            std::snprintf(buffer, sizeof(buffer), format, value);            \
        if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {   \
            (void)std::fputs(                                                \
                "Flecha runtime error: scalar formatting failed\n", stderr); \
            std::abort();                                                    \
        }                                                                    \
        flecha_runtime_v1_string_from_view(output, buffer,                   \
                                           static_cast<size_t>(length));     \
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
