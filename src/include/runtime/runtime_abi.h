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

// Internal state used by compiler-lowered __sprintf calls. This is not a
// language-level variadic container: code generation emits one typed append
// call for each source argument.
typedef struct FlechaRuntimeFormatBuffer {
    char* data;
    size_t length;
    size_t capacity;
    int32_t status;
} FlechaRuntimeFormatBuffer;

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

typedef enum FlechaRuntimeIoStatus {
    FLECHA_RUNTIME_IO_OK = 0,
    FLECHA_RUNTIME_IO_PERMISSION_DENIED = 2,
    FLECHA_RUNTIME_IO_TOO_LARGE = 5,
    FLECHA_RUNTIME_IO_INVALID_OPTIONS = 7,
    FLECHA_RUNTIME_IO_FAILED = 8,
} FlechaRuntimeIoStatus;

typedef enum FlechaRuntimeInputStatus {
    FLECHA_RUNTIME_INPUT_OK = 0,
    FLECHA_RUNTIME_INPUT_END_OF_FILE = 1,
    FLECHA_RUNTIME_INPUT_READ_FAILED = 2,
    FLECHA_RUNTIME_INPUT_INVALID_SYNTAX = 3,
    FLECHA_RUNTIME_INPUT_OUT_OF_RANGE = 4,
    FLECHA_RUNTIME_INPUT_PROMPT_FAILED = 5,
    FLECHA_RUNTIME_INPUT_OUT_OF_MEMORY = 6,
} FlechaRuntimeInputStatus;

typedef enum FlechaRuntimeProcessArgumentStatus {
    FLECHA_RUNTIME_PROCESS_ARGUMENT_OK = 0,
    FLECHA_RUNTIME_PROCESS_ARGUMENT_OUT_OF_MEMORY = 1,
    FLECHA_RUNTIME_PROCESS_ARGUMENT_INVALID_INDEX = 2,
} FlechaRuntimeProcessArgumentStatus;

typedef enum FlechaRuntimeEnvironmentStatus {
    FLECHA_RUNTIME_ENVIRONMENT_OK = 0,
    FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY = 1,
    FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME = 2,
    FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE = 3,
    FLECHA_RUNTIME_ENVIRONMENT_PERMISSION_DENIED = 4,
    FLECHA_RUNTIME_ENVIRONMENT_FAILED = 5,
} FlechaRuntimeEnvironmentStatus;

typedef enum FlechaRuntimeTimeStatus {
    FLECHA_RUNTIME_TIME_OK = 0,
    FLECHA_RUNTIME_TIME_UNAVAILABLE = 1,
    FLECHA_RUNTIME_TIME_OUT_OF_RANGE = 2,
    FLECHA_RUNTIME_TIME_INVALID_DURATION = 3,
    FLECHA_RUNTIME_TIME_FAILED = 4,
} FlechaRuntimeTimeStatus;

typedef enum FlechaRuntimePathStyle {
    FLECHA_RUNTIME_PATH_STYLE_POSIX = 0,
    FLECHA_RUNTIME_PATH_STYLE_WINDOWS = 1,
} FlechaRuntimePathStyle;

typedef enum FlechaRuntimeRandomStatus {
    FLECHA_RUNTIME_RANDOM_OK = 0,
    FLECHA_RUNTIME_RANDOM_UNAVAILABLE = 1,
    FLECHA_RUNTIME_RANDOM_FAILED = 2,
} FlechaRuntimeRandomStatus;

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

// The compiler-generated C entry wrapper installs process-lifetime argument
// storage before calling the Flecha main function. The runtime borrows argv;
// it never frees or mutates host-owned argument storage.
void flecha_runtime_v1_process_arguments_initialize(int32_t count,
                                                    const char* const* values);
size_t flecha_runtime_v1_process_argument_count(void);
// Copies one argument into an empty owning Flecha string. For a non-null
// output pointer, every failure leaves the output empty.
int32_t flecha_runtime_v1_process_argument_capture(FlechaRuntimeString* output,
                                                   size_t index);

// Fills the complete destination from the host cryptographically secure random
// source. A null destination is valid only when length is zero. On failure,
// any prefix may already have been overwritten and must not be used.
int32_t flecha_runtime_v1_secure_random_fill(uint8_t* destination,
                                             size_t length);

// Copies the current value into an empty owning Flecha string. A successful
// missing lookup sets present to zero; a successful empty value sets present
// to one and leaves output empty. Every failure clears both outputs.
int32_t flecha_runtime_v1_environment_get(FlechaRuntimeString* output,
                                          uint8_t* present, const char* name,
                                          size_t name_length);
// Mutates the process-wide environment. Names must be nonempty and may not
// contain '=' or a null byte. Values may be empty but may not contain null.
int32_t flecha_runtime_v1_environment_set(const char* name, size_t name_length,
                                          const char* value,
                                          size_t value_length);
// Removing an absent variable succeeds.
int32_t flecha_runtime_v1_environment_remove(const char* name,
                                             size_t name_length);

// Returns one normalized observation of the host monotonic clock. Every
// failure leaves both outputs zero.
int32_t flecha_runtime_v1_monotonic_clock_now(uint64_t* seconds,
                                              uint32_t* nanoseconds);
// Returns normalized Unix time. Every failure leaves both outputs zero.
int32_t flecha_runtime_v1_wall_clock_now(int64_t* unix_seconds,
                                         uint32_t* nanoseconds);
// Blocks the current thread for the normalized relative duration, retrying
// host interruptions. A duration outside the host range fails before waiting.
int32_t flecha_runtime_v1_sleep(uint64_t seconds, uint32_t nanoseconds);

// Reports the lexical path rules used by filesystem operations on this host.
uint8_t flecha_runtime_v1_path_style(void);

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
void flecha_runtime_v1_stderr_string_view(const char* data, size_t length);
void flecha_runtime_v1_stderr_i8(int8_t value);
void flecha_runtime_v1_stderr_i16(int16_t value);
void flecha_runtime_v1_stderr_i32(int32_t value);
void flecha_runtime_v1_stderr_i64(int64_t value);
void flecha_runtime_v1_stderr_isize(intptr_t value);
void flecha_runtime_v1_stderr_u8(uint8_t value);
void flecha_runtime_v1_stderr_u16(uint16_t value);
void flecha_runtime_v1_stderr_u32(uint32_t value);
void flecha_runtime_v1_stderr_u64(uint64_t value);
void flecha_runtime_v1_stderr_usize(uintptr_t value);
void flecha_runtime_v1_stderr_f32(float value);
void flecha_runtime_v1_stderr_f64(double value);
void flecha_runtime_v1_stderr_bool(uint8_t value);
void flecha_runtime_v1_stderr_char(uint8_t value);
void flecha_runtime_v1_stderr_newline(void);

// Flushes typed built-in output pending in the selected C stream, then performs
// one unbuffered descriptor write. Null storage is valid only for a zero-length
// write. The count output is reset before validation and preserves partial
// progress on failure.
int32_t flecha_runtime_v1_standard_output_write(const uint8_t* source,
                                                size_t length,
                                                size_t* bytes_written);
int32_t flecha_runtime_v1_standard_error_write(const uint8_t* source,
                                               size_t length,
                                               size_t* bytes_written);

// Flushes bytes emitted through the corresponding process-owned C stream.
int32_t flecha_runtime_v1_standard_output_flush(void);
int32_t flecha_runtime_v1_standard_error_flush(void);

// Performs one read from process standard input. Null storage is valid only
// for a zero-length read. The count output is reset before validation and
// preserves partial progress on failure. A zero count with success is EOF.
int32_t flecha_runtime_v1_standard_input_read(uint8_t* destination,
                                              size_t capacity,
                                              size_t* bytes_read);

// Reads one complete line, removing its LF and an immediately preceding CR.
// EOF before any byte is distinct from a successful final unterminated line.
// Success transfers one malloc-compatible owning string allocation.
int32_t flecha_runtime_v1_standard_input_read_line(FlechaRuntimeString* output);
int32_t flecha_runtime_v1_input_parse_i8(int8_t* output, const char* data,
                                         size_t length);
int32_t flecha_runtime_v1_input_parse_i16(int16_t* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_i32(int32_t* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_i64(int64_t* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_isize(intptr_t* output, const char* data,
                                            size_t length);
int32_t flecha_runtime_v1_input_parse_u8(uint8_t* output, const char* data,
                                         size_t length);
int32_t flecha_runtime_v1_input_parse_u16(uint16_t* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_u32(uint32_t* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_u64(uint64_t* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_usize(uintptr_t* output, const char* data,
                                            size_t length);
int32_t flecha_runtime_v1_input_parse_f32(float* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_f64(double* output, const char* data,
                                          size_t length);
int32_t flecha_runtime_v1_input_parse_bool(uint8_t* output, const char* data,
                                           size_t length);
int32_t flecha_runtime_v1_input_parse_char(uint8_t* output, const char* data,
                                           size_t length);
uint8_t flecha_runtime_v1_string_view_at(const char* data, size_t length,
                                         uint32_t index);
void flecha_runtime_v1_string_view_check_substr(size_t length, uint32_t start,
                                                uint32_t count);
// Copies bytes into null-terminated owned string storage. Empty input succeeds
// without allocating. Every failure resets output to the empty string.
int32_t flecha_runtime_v1_string_try_from_bytes(FlechaRuntimeString* output,
                                                const uint8_t* data,
                                                size_t length);
void flecha_runtime_v1_format_buffer_init(FlechaRuntimeFormatBuffer* buffer);
void flecha_runtime_v1_format_buffer_append_string_view(
    FlechaRuntimeFormatBuffer* buffer, const char* data, size_t length);
void flecha_runtime_v1_format_buffer_append_i8(
    FlechaRuntimeFormatBuffer* buffer, int8_t value);
void flecha_runtime_v1_format_buffer_append_i16(
    FlechaRuntimeFormatBuffer* buffer, int16_t value);
void flecha_runtime_v1_format_buffer_append_i32(
    FlechaRuntimeFormatBuffer* buffer, int32_t value);
void flecha_runtime_v1_format_buffer_append_i64(
    FlechaRuntimeFormatBuffer* buffer, int64_t value);
void flecha_runtime_v1_format_buffer_append_isize(
    FlechaRuntimeFormatBuffer* buffer, intptr_t value);
void flecha_runtime_v1_format_buffer_append_u8(
    FlechaRuntimeFormatBuffer* buffer, uint8_t value);
void flecha_runtime_v1_format_buffer_append_u16(
    FlechaRuntimeFormatBuffer* buffer, uint16_t value);
void flecha_runtime_v1_format_buffer_append_u32(
    FlechaRuntimeFormatBuffer* buffer, uint32_t value);
void flecha_runtime_v1_format_buffer_append_u64(
    FlechaRuntimeFormatBuffer* buffer, uint64_t value);
void flecha_runtime_v1_format_buffer_append_usize(
    FlechaRuntimeFormatBuffer* buffer, uintptr_t value);
void flecha_runtime_v1_format_buffer_append_f32(
    FlechaRuntimeFormatBuffer* buffer, float value);
void flecha_runtime_v1_format_buffer_append_f64(
    FlechaRuntimeFormatBuffer* buffer, double value);
void flecha_runtime_v1_format_buffer_append_bool(
    FlechaRuntimeFormatBuffer* buffer, uint8_t value);
void flecha_runtime_v1_format_buffer_append_char(
    FlechaRuntimeFormatBuffer* buffer, uint8_t value);
// Transfers the completed allocation to output. On failure, releases partial
// storage, resets output, and returns FLECHA_RUNTIME_STRING_OUT_OF_MEMORY.
int32_t flecha_runtime_v1_format_buffer_finish(
    FlechaRuntimeFormatBuffer* buffer, FlechaRuntimeString* output);
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
// resets written before validation, and leaves destination unchanged on
// failure.
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
// Resolves existing path components and reports whether child is equal to or
// below parent. parent must exist; child may name a not-yet-created entry.
// Symbolic links in either path are resolved. output is reset before
// validation.
int32_t flecha_runtime_v1_path_contains(uint8_t* output, const char* parent,
                                        size_t parent_length, const char* child,
                                        size_t child_length);
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

// Atomically creates a private directory in the host temporary directory. On
// success output_path owns its absolute host path (native bytes on POSIX and
// UTF-8 on Windows). Every failure removes any partially created directory and
// resets the output.
int32_t flecha_runtime_v1_temporary_directory_create(
    FlechaRuntimeString* output_path);

// Opens an owning directory iterator. Paths follow read_file's platform
// rules. Every failure leaves output null.
int32_t flecha_runtime_v1_directory_open(void** output, const char* path,
                                         size_t path_length);

// Returns one entry name and its non-following type: 0 for other, 1 for a
// regular file, 2 for a directory, and 3 for a symbolic link or Windows
// reparse-point link. End of iteration is successful with has_entry zero.
// output_name must point to an empty Flecha string. All outputs are reset
// before validation. Allocation failure preserves the pending entry so a later
// call can retry it.
int32_t flecha_runtime_v1_directory_next(void* handle,
                                         FlechaRuntimeString* output_name,
                                         uint8_t* output_type,
                                         uint8_t* output_has_entry);

// Closes and destroys a handle returned by directory_open. The handle is
// consumed even when the underlying close operation reports a failure.
int32_t flecha_runtime_v1_directory_close(void* handle);

// Opens an unbuffered binary file stream. Paths follow read_file's platform
// rules. Every option is a byte boolean and must be zero or one. On success,
// output receives an opaque runtime-owned handle; every failure leaves it null.
int32_t flecha_runtime_v1_file_stream_open(void** output, const char* path,
                                           size_t path_length, uint8_t read,
                                           uint8_t write, uint8_t append,
                                           uint8_t truncate, uint8_t create,
                                           uint8_t create_new);

// Atomically creates a private temporary file in the host temporary directory.
// On success output_handle owns a readable and writable file stream and
// output_path owns its absolute host path (native bytes on POSIX and UTF-8 on
// Windows). Every failure closes and removes any partially created file and
// resets both outputs.
int32_t flecha_runtime_v1_temporary_file_open(void** output_handle,
                                              FlechaRuntimeString* output_path);

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
