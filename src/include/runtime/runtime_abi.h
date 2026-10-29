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

typedef enum FlechaRuntimeStringStatus {
    FLECHA_RUNTIME_STRING_OK = 0,
    FLECHA_RUNTIME_STRING_OUT_OF_MEMORY = 1,
} FlechaRuntimeStringStatus;

typedef enum FlechaRuntimeFloatTextStatus {
    FLECHA_RUNTIME_FLOAT_TEXT_OK = 0,
    FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX = 1,
    FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_RANGE = 2,
    FLECHA_RUNTIME_FLOAT_TEXT_INSUFFICIENT_SPACE = 3,
    FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_MEMORY = 4,
} FlechaRuntimeFloatTextStatus;

typedef enum FlechaRuntimeReadFileStatus {
    FLECHA_RUNTIME_READ_FILE_OK = 0,
    FLECHA_RUNTIME_READ_FILE_NOT_FOUND = 1,
    FLECHA_RUNTIME_READ_FILE_PERMISSION_DENIED = 2,
    FLECHA_RUNTIME_READ_FILE_INVALID_PATH = 3,
    FLECHA_RUNTIME_READ_FILE_FAILED = 4,
    FLECHA_RUNTIME_READ_FILE_TOO_LARGE = 5,
    FLECHA_RUNTIME_READ_FILE_OUT_OF_MEMORY = 6,
} FlechaRuntimeReadFileStatus;

typedef enum FlechaRuntimeFileStreamStatus {
    FLECHA_RUNTIME_FILE_STREAM_OK = 0,
    FLECHA_RUNTIME_FILE_STREAM_NOT_FOUND = 1,
    FLECHA_RUNTIME_FILE_STREAM_PERMISSION_DENIED = 2,
    FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH = 3,
    FLECHA_RUNTIME_FILE_STREAM_ALREADY_EXISTS = 4,
    FLECHA_RUNTIME_FILE_STREAM_TOO_LARGE = 5,
    FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY = 6,
    FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS = 7,
    FLECHA_RUNTIME_FILE_STREAM_FAILED = 8,
} FlechaRuntimeFileStreamStatus;

typedef enum FlechaRuntimeFilesystemStatus {
    FLECHA_RUNTIME_FILESYSTEM_OK = 0,
    FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND = 1,
    FLECHA_RUNTIME_FILESYSTEM_PERMISSION_DENIED = 2,
    FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH = 3,
    FLECHA_RUNTIME_FILESYSTEM_ALREADY_EXISTS = 4,
    FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE = 5,
    FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY = 6,
    FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS = 7,
    FLECHA_RUNTIME_FILESYSTEM_FAILED = 8,
    FLECHA_RUNTIME_FILESYSTEM_DIRECTORY_NOT_EMPTY = 9,
} FlechaRuntimeFilesystemStatus;

// Allocates zeroed aligned storage; returns null on failure. Free accepts null.
// Only pointers returned by this allocator may be released. The compiler tears
// down the object before releasing storage.
void* flecha_runtime_v1_alloc(size_t size, size_t alignment);
// Terminates execution after an allocation that cannot be represented as a
// language-level error, such as closure-environment creation.
void flecha_runtime_v1_allocation_failure(void);
// Allocates count zeroed elements, checking multiplication before allocation.
// Zero count and zero-sized elements still return a releasable allocation on
// success. Failure returns null and does not allocate storage.
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
// Copies bytes into null-terminated owned string storage. Empty input succeeds
// without allocating. Every failure resets output to the empty string.
int32_t flecha_runtime_v1_string_try_from_bytes(FlechaRuntimeString* output,
                                                const uint8_t* data,
                                                size_t length);
void flecha_runtime_v1_string_from_view(FlechaRuntimeString* output,
                                        const char* data, size_t length);
void flecha_runtime_v1_string_clone(FlechaRuntimeString* output,
                                    const char* data, size_t length);
void flecha_runtime_v1_string_concat(FlechaRuntimeString* output,
                                     const char* leftData, size_t leftLength,
                                     const char* rightData, size_t rightLength);
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

// Parses the complete locale-independent decimal input. Every failure resets
// output to zero. Formatting emits the shortest round-trippable representation,
// resets written before validation, and leaves destination unchanged on failure.
int32_t flecha_runtime_v1_parse_f32(float* output, const char* data,
                                    size_t length);
int32_t flecha_runtime_v1_parse_f64(double* output, const char* data,
                                    size_t length);
int32_t flecha_runtime_v1_format_f32(uint8_t* destination, size_t capacity,
                                     float value, size_t* written);
int32_t flecha_runtime_v1_format_f64(uint8_t* destination, size_t capacity,
                                     double value, size_t* written);

// Reads path as platform-native binary file contents. Paths are byte strings
// on POSIX and UTF-8 on Windows. On success output owns a malloc-compatible,
// null-terminated buffer. On failure output is empty.
int32_t flecha_runtime_v1_read_file(FlechaRuntimeString* output,
                                    const char* path, size_t path_length);

// Filesystem paths follow read_file's platform rules. path_exists reports a
// missing path as successful false; the mutation operations report it as
// NOT_FOUND. Every path is validated before an operating-system call.
int32_t flecha_runtime_v1_path_exists(uint8_t* output, const char* path,
                                      size_t path_length);
// Metadata follows symbolic links. type is 0 for other entries, 1 for regular
// files, and 2 for directories. size is the regular-file byte length and zero
// for every other type. Both outputs are reset before path validation.
int32_t flecha_runtime_v1_metadata(uint8_t* output_type, uint64_t* output_size,
                                   const char* path, size_t path_length);
int32_t flecha_runtime_v1_remove_file(const char* path, size_t path_length);
int32_t flecha_runtime_v1_rename(const char* source, size_t source_length,
                                 const char* destination,
                                 size_t destination_length);
int32_t flecha_runtime_v1_create_directory(const char* path,
                                           size_t path_length);
int32_t flecha_runtime_v1_remove_directory(const char* path, size_t path_length,
                                           uint8_t force, uint8_t recursive);

// Opens an unbuffered binary file stream. Paths follow read_file's platform
// rules. Every option is a byte boolean and must be zero or one. On success,
// output receives an opaque runtime-owned handle; every failure leaves it null.
int32_t flecha_runtime_v1_file_stream_open(void** output, const char* path,
                                           size_t path_length, uint8_t read,
                                           uint8_t write, uint8_t append,
                                           uint8_t truncate, uint8_t create,
                                           uint8_t create_new);

// Closes and destroys a handle returned by file_stream_open. The handle is
// consumed even when the underlying close operation reports a failure.
int32_t flecha_runtime_v1_file_stream_close(void* handle);

// Performs one system read/write operation. Null storage is valid only when
// its corresponding size is zero. Count outputs are reset before validation.
int32_t flecha_runtime_v1_file_stream_read(void* handle, uint8_t* destination,
                                           size_t capacity, size_t* bytes_read);
int32_t flecha_runtime_v1_file_stream_write(void* handle, const uint8_t* source,
                                            size_t length,
                                            size_t* bytes_written);

// origin is 0 for start, 1 for current, and 2 for end. On success position is
// the absolute byte offset from the start of the file.
int32_t flecha_runtime_v1_file_stream_seek(void* handle, int64_t offset,
                                           uint8_t origin, uint64_t* position);

// Synchronizes pending writes with the backing file. Read-only handles succeed
// without issuing a system flush.
int32_t flecha_runtime_v1_file_stream_flush(void* handle);

#ifdef __cplusplus
}
#endif

#endif  // FLECHA_RUNTIME_RUNTIME_ABI_H
