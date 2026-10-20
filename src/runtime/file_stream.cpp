#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

struct FileStream final {
    int descriptor;
    bool readable;
    bool writable;
};

bool IsByteBoolean(uint8_t value) { return value <= 1; }

int32_t MapOpenError(int error) {
    switch (error) {
        case ENOENT:
#if defined(ENOTDIR) && ENOTDIR != ENOENT
        case ENOTDIR:
#endif
            return FLECHA_RUNTIME_FILE_STREAM_NOT_FOUND;
        case EACCES:
#if defined(EPERM) && EPERM != EACCES
        case EPERM:
#endif
#if defined(EROFS) && EROFS != EACCES && EROFS != EPERM
        case EROFS:
#endif
            return FLECHA_RUNTIME_FILE_STREAM_PERMISSION_DENIED;
        case EEXIST:
            return FLECHA_RUNTIME_FILE_STREAM_ALREADY_EXISTS;
#if defined(ENAMETOOLONG) && ENAMETOOLONG != EINVAL
        case ENAMETOOLONG:
#endif
        case EINVAL:
            return FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
#if defined(ENOMEM)
        case ENOMEM:
            return FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
#endif
        default:
            return FLECHA_RUNTIME_FILE_STREAM_FAILED;
    }
}

int32_t MapOperationError(int error) {
    switch (error) {
        case EACCES:
#if defined(EPERM) && EPERM != EACCES
        case EPERM:
#endif
#if defined(EROFS) && EROFS != EACCES && EROFS != EPERM
        case EROFS:
#endif
            return FLECHA_RUNTIME_FILE_STREAM_PERMISSION_DENIED;
#if defined(EFBIG)
        case EFBIG:
#endif
#if defined(EOVERFLOW) && EOVERFLOW != EFBIG
        case EOVERFLOW:
#endif
            return FLECHA_RUNTIME_FILE_STREAM_TOO_LARGE;
#if defined(ENOMEM)
        case ENOMEM:
            return FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
#endif
        default:
            return FLECHA_RUNTIME_FILE_STREAM_FAILED;
    }
}

bool ValidOptions(uint8_t read, uint8_t write, uint8_t append, uint8_t truncate,
                  uint8_t create, uint8_t createNew) {
    if (!IsByteBoolean(read) || !IsByteBoolean(write) ||
        !IsByteBoolean(append) || !IsByteBoolean(truncate) ||
        !IsByteBoolean(create) || !IsByteBoolean(createNew))
        return false;
    if (read == 0 && write == 0) return false;
    if ((append != 0 || truncate != 0 || create != 0 || createNew != 0) &&
        write == 0)
        return false;
    return true;
}

int OpenDescriptor(const char* path, size_t pathLength, uint8_t read,
                   uint8_t write, uint8_t append, uint8_t truncate,
                   uint8_t create, uint8_t createNew, int32_t* status) {
#ifdef _WIN32
    if (pathLength > static_cast<size_t>(INT_MAX)) {
        *status = FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
        return -1;
    }
    const int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            static_cast<int>(pathLength), nullptr, 0);
    if (wideLength <= 0) {
        *status = FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
        return -1;
    }
    auto* widePath = static_cast<wchar_t*>(
        std::malloc((static_cast<size_t>(wideLength) + 1) * sizeof(wchar_t)));
    if (widePath == nullptr) {
        *status = FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
        return -1;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            static_cast<int>(pathLength), widePath,
                            wideLength) != wideLength) {
        std::free(widePath);
        *status = FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
        return -1;
    }
    widePath[wideLength] = L'\0';

    int flags = _O_BINARY | _O_NOINHERIT;
    flags |= read != 0 && write != 0 ? _O_RDWR
             : read != 0             ? _O_RDONLY
                                     : _O_WRONLY;
    if (append != 0) flags |= _O_APPEND;
    if (truncate != 0) flags |= _O_TRUNC;
    if (create != 0 || createNew != 0) flags |= _O_CREAT;
    if (createNew != 0) flags |= _O_EXCL;

    errno = 0;
    const int descriptor = _wopen(widePath, flags, _S_IREAD | _S_IWRITE);
    const int openError = errno;
    std::free(widePath);
    if (descriptor < 0) *status = MapOpenError(openError);
    return descriptor;
#else
    if (pathLength == std::numeric_limits<size_t>::max()) {
        *status = FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
        return -1;
    }
    auto* terminated = static_cast<char*>(std::malloc(pathLength + 1));
    if (terminated == nullptr) {
        *status = FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
        return -1;
    }
    std::memcpy(terminated, path, pathLength);
    terminated[pathLength] = '\0';

    int flags = read != 0 && write != 0 ? O_RDWR
                : read != 0             ? O_RDONLY
                                        : O_WRONLY;
    if (append != 0) flags |= O_APPEND;
    if (truncate != 0) flags |= O_TRUNC;
    if (create != 0 || createNew != 0) flags |= O_CREAT;
    if (createNew != 0) flags |= O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    errno = 0;
    const int descriptor = ::open(terminated, flags, 0666);
    const int openError = errno;
    std::free(terminated);
    if (descriptor < 0) *status = MapOpenError(openError);
    return descriptor;
#endif
}

int CloseDescriptor(int descriptor) {
#ifdef _WIN32
    return _close(descriptor);
#else
    return ::close(descriptor);
#endif
}

}  // namespace

extern "C" int32_t flecha_runtime_v1_file_stream_open(
    void** output, const char* path, size_t pathLength, uint8_t read,
    uint8_t write, uint8_t append, uint8_t truncate, uint8_t create,
    uint8_t createNew) {
    if (output == nullptr) return FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
    *output = nullptr;
    if (path == nullptr || pathLength == 0 ||
        std::memchr(path, '\0', pathLength) != nullptr)
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
    if (!ValidOptions(read, write, append, truncate, create, createNew))
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;

    int32_t status = FLECHA_RUNTIME_FILE_STREAM_OK;
    const int descriptor = OpenDescriptor(path, pathLength, read, write, append,
                                          truncate, create, createNew, &status);
    if (descriptor < 0) return status;

    auto* stream = static_cast<FileStream*>(std::malloc(sizeof(FileStream)));
    if (stream == nullptr) {
        (void)CloseDescriptor(descriptor);
        return FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
    }
    *stream = {descriptor, read != 0, write != 0};
    *output = stream;
    return FLECHA_RUNTIME_FILE_STREAM_OK;
}

extern "C" int32_t flecha_runtime_v1_file_stream_close(void* handle) {
    if (handle == nullptr) return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    auto* stream = static_cast<FileStream*>(handle);
    errno = 0;
    const int result = CloseDescriptor(stream->descriptor);
    const int closeError = errno;
    std::free(stream);
    return result == 0 ? FLECHA_RUNTIME_FILE_STREAM_OK
                       : MapOperationError(closeError);
}

extern "C" int32_t flecha_runtime_v1_file_stream_read(void* handle,
                                                      uint8_t* destination,
                                                      size_t capacity,
                                                      size_t* bytesRead) {
    if (bytesRead == nullptr) return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    *bytesRead = 0;
    if (handle == nullptr || (destination == nullptr && capacity != 0))
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    auto* stream = static_cast<FileStream*>(handle);
    if (!stream->readable) return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    if (capacity == 0) return FLECHA_RUNTIME_FILE_STREAM_OK;

#ifdef _WIN32
    const unsigned int request = static_cast<unsigned int>(std::min(
        capacity, static_cast<size_t>(std::numeric_limits<int>::max())));
    int result;
    do {
        errno = 0;
        result = _read(stream->descriptor, destination, request);
    } while (result < 0 && errno == EINTR);
#else
    const size_t request = std::min(
        capacity, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    ssize_t result;
    do {
        errno = 0;
        result = ::read(stream->descriptor, destination, request);
    } while (result < 0 && errno == EINTR);
#endif
    if (result < 0) return MapOperationError(errno);
    *bytesRead = static_cast<size_t>(result);
    return FLECHA_RUNTIME_FILE_STREAM_OK;
}

extern "C" int32_t flecha_runtime_v1_file_stream_write(void* handle,
                                                       const uint8_t* source,
                                                       size_t length,
                                                       size_t* bytesWritten) {
    if (bytesWritten == nullptr)
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    *bytesWritten = 0;
    if (handle == nullptr || (source == nullptr && length != 0))
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    auto* stream = static_cast<FileStream*>(handle);
    if (!stream->writable) return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    if (length == 0) return FLECHA_RUNTIME_FILE_STREAM_OK;

#ifdef _WIN32
    const unsigned int request = static_cast<unsigned int>(
        std::min(length, static_cast<size_t>(std::numeric_limits<int>::max())));
    int result;
    do {
        errno = 0;
        result = _write(stream->descriptor, source, request);
    } while (result < 0 && errno == EINTR);
#else
    const size_t request = std::min(
        length, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    ssize_t result;
    do {
        errno = 0;
        result = ::write(stream->descriptor, source, request);
    } while (result < 0 && errno == EINTR);
#endif
    if (result < 0) return MapOperationError(errno);
    *bytesWritten = static_cast<size_t>(result);
    return FLECHA_RUNTIME_FILE_STREAM_OK;
}

extern "C" int32_t flecha_runtime_v1_file_stream_seek(void* handle,
                                                      int64_t offset,
                                                      uint8_t origin,
                                                      uint64_t* position) {
    if (position == nullptr) return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    *position = 0;
    if (handle == nullptr || origin > 2)
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    auto* stream = static_cast<FileStream*>(handle);
    const int nativeOrigin = origin == 0   ? SEEK_SET
                             : origin == 1 ? SEEK_CUR
                                           : SEEK_END;

#ifdef _WIN32
    errno = 0;
    const __int64 result = _lseeki64(
        stream->descriptor, static_cast<__int64>(offset), nativeOrigin);
    if (result < 0) return MapOperationError(errno);
#else
    if (!std::in_range<off_t>(offset))
        return FLECHA_RUNTIME_FILE_STREAM_TOO_LARGE;
    errno = 0;
    const off_t result =
        ::lseek(stream->descriptor, static_cast<off_t>(offset), nativeOrigin);
    if (result < 0) return MapOperationError(errno);
#endif
    *position = static_cast<uint64_t>(result);
    return FLECHA_RUNTIME_FILE_STREAM_OK;
}

extern "C" int32_t flecha_runtime_v1_file_stream_flush(void* handle) {
    if (handle == nullptr) return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;
    auto* stream = static_cast<FileStream*>(handle);
    if (!stream->writable) return FLECHA_RUNTIME_FILE_STREAM_OK;

    int result;
    do {
        errno = 0;
#ifdef _WIN32
        result = _commit(stream->descriptor);
#else
        result = ::fsync(stream->descriptor);
#endif
    } while (result != 0 && errno == EINTR);
    return result == 0 ? FLECHA_RUNTIME_FILE_STREAM_OK
                       : MapOperationError(errno);
}
