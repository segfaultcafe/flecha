#ifndef FLECHA_RUNTIME_RUNTIME_ABI_H
#define FLECHA_RUNTIME_RUNTIME_ABI_H

#include <stddef.h>
#include <stdint.h>

#define FLECHA_RUNTIME_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlechaRuntimeString {
    char* data;
    size_t length;
    size_t capacity;
} FlechaRuntimeString;

typedef enum FlechaRuntimeReadFileStatus {
    FLECHA_RUNTIME_READ_FILE_OK = 0,
    FLECHA_RUNTIME_READ_FILE_NOT_FOUND = 1,
    FLECHA_RUNTIME_READ_FILE_PERMISSION_DENIED = 2,
    FLECHA_RUNTIME_READ_FILE_INVALID_PATH = 3,
    FLECHA_RUNTIME_READ_FILE_FAILED = 4,
    FLECHA_RUNTIME_READ_FILE_TOO_LARGE = 5,
    FLECHA_RUNTIME_READ_FILE_OUT_OF_MEMORY = 6,
} FlechaRuntimeReadFileStatus;

// Allocates zeroed aligned storage; returns null on failure. Free accepts null.
// Only pointers returned by this allocator may be released. The compiler tears
// down the object before releasing storage.
void* flecha_runtime_v1_alloc(size_t size, size_t alignment);
// Terminates execution after an allocation that cannot be represented as a
// language-level error, such as closure-environment creation.
void flecha_runtime_v1_allocation_failure(void);
// Allocates count zeroed elements, checking multiplication before allocation.
// Zero count still returns a releasable allocation on success. Zero element
// size is invalid. Failure returns null and does not allocate storage.
void* flecha_runtime_v1_alloc_array(size_t count, size_t element_size,
                                    size_t alignment);
// Requires an allocation base returned by this runtime, or null.
size_t flecha_runtime_v1_allocation_count(const void* data);
void flecha_runtime_v1_free(void* data);

void flecha_runtime_v1_stdout_string_view(const char* data, size_t length);
void flecha_runtime_v1_stdout_i8(int8_t value);
void flecha_runtime_v1_stdout_i16(int16_t value);
void flecha_runtime_v1_stdout_i32(int32_t value);
void flecha_runtime_v1_stdout_i64(int64_t value);
void flecha_runtime_v1_stdout_isize(intptr_t value);
void flecha_runtime_v1_stdout_u8(uint8_t value);
void flecha_runtime_v1_stdout_u16(uint16_t value);
void flecha_runtime_v1_stdout_u32(uint32_t value);
void flecha_runtime_v1_stdout_u64(uint64_t value);
void flecha_runtime_v1_stdout_usize(uintptr_t value);
void flecha_runtime_v1_stdout_f32(float value);
void flecha_runtime_v1_stdout_f64(double value);
void flecha_runtime_v1_stdout_bool(uint8_t value);
void flecha_runtime_v1_stdout_char(uint8_t value);
void flecha_runtime_v1_stdout_newline(void);
uint8_t flecha_runtime_v1_string_view_at(const char* data, size_t length,
                                         uint32_t index);
void flecha_runtime_v1_string_view_check_substr(size_t length, uint32_t start,
                                                uint32_t count);
void flecha_runtime_v1_string_from_view(FlechaRuntimeString* output,
                                        const char* data, size_t length);
void flecha_runtime_v1_string_clone(FlechaRuntimeString* output,
                                    const char* data, size_t length);
void flecha_runtime_v1_string_concat(FlechaRuntimeString* output,
                                     const char* leftData, size_t leftLength,
                                     const char* rightData,
                                     size_t rightLength);
void flecha_runtime_v1_string_destroy(char* data);
void flecha_runtime_v1_string_from_i8(FlechaRuntimeString* output,
                                      int8_t value);
void flecha_runtime_v1_string_from_i16(FlechaRuntimeString* output,
                                       int16_t value);
void flecha_runtime_v1_string_from_i32(FlechaRuntimeString* output,
                                       int32_t value);
void flecha_runtime_v1_string_from_i64(FlechaRuntimeString* output,
                                       int64_t value);
void flecha_runtime_v1_string_from_isize(FlechaRuntimeString* output,
                                         intptr_t value);
void flecha_runtime_v1_string_from_u8(FlechaRuntimeString* output,
                                      uint8_t value);
void flecha_runtime_v1_string_from_u16(FlechaRuntimeString* output,
                                       uint16_t value);
void flecha_runtime_v1_string_from_u32(FlechaRuntimeString* output,
                                       uint32_t value);
void flecha_runtime_v1_string_from_u64(FlechaRuntimeString* output,
                                       uint64_t value);
void flecha_runtime_v1_string_from_usize(FlechaRuntimeString* output,
                                         uintptr_t value);
void flecha_runtime_v1_string_from_f32(FlechaRuntimeString* output,
                                       float value);
void flecha_runtime_v1_string_from_f64(FlechaRuntimeString* output,
                                       double value);
void flecha_runtime_v1_string_from_bool(FlechaRuntimeString* output,
                                        uint8_t value);
void flecha_runtime_v1_string_from_char(FlechaRuntimeString* output,
                                        uint8_t value);

// Reads path as platform-native binary file contents. Paths are byte strings
// on POSIX and UTF-8 on Windows. On success output owns a malloc-compatible,
// null-terminated buffer. On failure output is empty.
int32_t flecha_runtime_v1_read_file(FlechaRuntimeString* output,
                                    const char* path, size_t path_length);

#ifdef __cplusplus
}
#endif

#endif  // FLECHA_RUNTIME_RUNTIME_ABI_H
