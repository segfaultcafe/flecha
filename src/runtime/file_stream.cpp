#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
// BCrypt declarations depend on base Win32 types. Keep this order even when
// formatting the translation unit.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
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

int32_t MapOpenError(const std::error_code& error) {
    const std::error_condition condition = error.default_error_condition();
    if (condition == std::errc::no_such_file_or_directory ||
        condition == std::errc::not_a_directory)
        return FLECHA_RUNTIME_FILE_STREAM_NOT_FOUND;
    if (condition == std::errc::permission_denied ||
        condition == std::errc::read_only_file_system)
        return FLECHA_RUNTIME_FILE_STREAM_PERMISSION_DENIED;
    if (condition == std::errc::file_exists)
        return FLECHA_RUNTIME_FILE_STREAM_ALREADY_EXISTS;
    if (condition == std::errc::invalid_argument ||
        condition == std::errc::filename_too_long)
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH;
    if (condition == std::errc::file_too_large ||
        condition == std::errc::value_too_large)
        return FLECHA_RUNTIME_FILE_STREAM_TOO_LARGE;
    if (condition == std::errc::not_enough_memory)
        return FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
    return FLECHA_RUNTIME_FILE_STREAM_FAILED;
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

void RemoveTemporaryPath(const std::filesystem::path& path) {
    if (path.empty()) return;
#ifdef _WIN32
    (void)_wunlink(path.c_str());
#else
    (void)::unlink(path.c_str());
#endif
}

void RemoveTemporaryDirectoryPath(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ignored;
    (void)std::filesystem::remove_all(path, ignored);
}

#ifdef _WIN32
bool RandomTemporaryName(bool directory, std::wstring* output) {
    std::array<unsigned char, 16> random{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random.data(),
                                        static_cast<ULONG>(random.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return false;

    constexpr wchar_t HEX[] = L"0123456789abcdef";
    *output = L"flecha-";
    output->reserve(7 + random.size() * 2 + 4);
    for (const unsigned char byte : random) {
        output->push_back(HEX[byte >> 4]);
        output->push_back(HEX[byte & 0x0f]);
    }
    output->append(directory ? L".dir" : L".tmp");
    return true;
}
#endif

int CreateTemporaryDescriptor(std::filesystem::path* outputPath,
                              int32_t* status) {
    std::error_code pathError;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(pathError);
    if (pathError) {
        *status = MapOpenError(pathError);
        return -1;
    }
    if (!root.is_absolute()) {
        root = std::filesystem::absolute(root, pathError);
        if (pathError) {
            *status = MapOpenError(pathError);
            return -1;
        }
    }

#ifdef _WIN32
    constexpr unsigned int MAX_ATTEMPTS = 128;
    for (unsigned int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        std::wstring name;
        if (!RandomTemporaryName(false, &name)) {
            *status = FLECHA_RUNTIME_FILE_STREAM_FAILED;
            return -1;
        }
        std::filesystem::path candidate = root / name;
        errno = 0;
        const int descriptor =
            _wopen(candidate.c_str(),
                   _O_BINARY | _O_NOINHERIT | _O_RDWR | _O_CREAT | _O_EXCL,
                   _S_IREAD | _S_IWRITE);
        if (descriptor >= 0) {
            outputPath->swap(candidate);
            return descriptor;
        }
        if (errno != EEXIST) {
            *status = MapOpenError(errno);
            return -1;
        }
    }
    *status = FLECHA_RUNTIME_FILE_STREAM_ALREADY_EXISTS;
    return -1;
#else
    std::string candidate = (root / "flecha-XXXXXX").native();
    errno = 0;
    const int descriptor = ::mkstemp(candidate.data());
    if (descriptor < 0) {
        *status = MapOpenError(errno);
        return -1;
    }

    errno = 0;
    const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
    if (descriptorFlags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
        const int flagError = errno;
        (void)CloseDescriptor(descriptor);
        (void)::unlink(candidate.c_str());
        *status = MapOperationError(flagError);
        return -1;
    }

    try {
        std::filesystem::path createdPath(candidate);
        outputPath->swap(createdPath);
    } catch (...) {
        (void)CloseDescriptor(descriptor);
        (void)::unlink(candidate.c_str());
        throw;
    }
    return descriptor;
#endif
}

int32_t CreateTemporaryDirectoryPath(std::filesystem::path* outputPath) {
    std::error_code pathError;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(pathError);
    if (pathError) return MapOpenError(pathError);
    if (!root.is_absolute()) {
        root = std::filesystem::absolute(root, pathError);
        if (pathError) return MapOpenError(pathError);
    }

#ifdef _WIN32
    constexpr unsigned int MAX_ATTEMPTS = 128;
    for (unsigned int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        std::wstring name;
        if (!RandomTemporaryName(true, &name))
            return FLECHA_RUNTIME_FILESYSTEM_FAILED;

        std::filesystem::path candidate = root / name;
        std::error_code creationError;
        if (std::filesystem::create_directory(candidate, creationError)) {
            outputPath->swap(candidate);
            return FLECHA_RUNTIME_FILESYSTEM_OK;
        }
        if (creationError &&
            creationError.default_error_condition() != std::errc::file_exists)
            return MapOpenError(creationError);
    }
    return FLECHA_RUNTIME_FILESYSTEM_ALREADY_EXISTS;
#else
    std::string candidate = (root / "flecha-XXXXXX").native();
    errno = 0;
    if (::mkdtemp(candidate.data()) == nullptr) return MapOpenError(errno);

    try {
        std::filesystem::path createdPath(candidate);
        outputPath->swap(createdPath);
    } catch (...) {
        (void)::rmdir(candidate.c_str());
        throw;
    }
    return FLECHA_RUNTIME_FILESYSTEM_OK;
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

extern "C" int32_t flecha_runtime_v1_temporary_file_open(
    void** outputHandle, FlechaRuntimeString* outputPath) {
    if (outputHandle != nullptr) *outputHandle = nullptr;
    if (outputPath != nullptr) *outputPath = {nullptr, 0, 0};
    if (outputHandle == nullptr || outputPath == nullptr)
        return FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS;

    int descriptor = -1;
    std::filesystem::path path;
    int32_t status = FLECHA_RUNTIME_FILE_STREAM_OK;
    FileStream* stream = nullptr;
    try {
        descriptor = CreateTemporaryDescriptor(&path, &status);
        if (descriptor < 0) return status;

        stream = static_cast<FileStream*>(std::malloc(sizeof(FileStream)));
        if (stream == nullptr) {
            status = FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
        } else {
            *stream = {descriptor, true, true};
            const std::u8string encodedPath = path.u8string();
            const int32_t stringStatus =
                flecha_runtime_v1_string_try_from_bytes(
                    outputPath,
                    reinterpret_cast<const uint8_t*>(encodedPath.data()),
                    encodedPath.size());
            if (stringStatus != FLECHA_RUNTIME_STRING_OK)
                status = FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
        }
    } catch (const std::bad_alloc&) {
        status = FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY;
    } catch (...) {
        status = FLECHA_RUNTIME_FILE_STREAM_FAILED;
    }

    if (status != FLECHA_RUNTIME_FILE_STREAM_OK) {
        std::free(stream);
        if (descriptor >= 0) (void)CloseDescriptor(descriptor);
        RemoveTemporaryPath(path);
        flecha_runtime_v1_string_destroy(outputPath->data);
        *outputPath = {nullptr, 0, 0};
        return status;
    }

    *outputHandle = stream;
    return FLECHA_RUNTIME_FILE_STREAM_OK;
}

extern "C" int32_t flecha_runtime_v1_temporary_directory_create(
    FlechaRuntimeString* outputPath) {
    if (outputPath != nullptr) *outputPath = {nullptr, 0, 0};
    if (outputPath == nullptr) return FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS;

    std::filesystem::path path;
    int32_t status = FLECHA_RUNTIME_FILESYSTEM_OK;
    try {
        status = CreateTemporaryDirectoryPath(&path);
        if (status == FLECHA_RUNTIME_FILESYSTEM_OK) {
            const std::u8string encodedPath = path.u8string();
            const int32_t stringStatus =
                flecha_runtime_v1_string_try_from_bytes(
                    outputPath,
                    reinterpret_cast<const uint8_t*>(encodedPath.data()),
                    encodedPath.size());
            if (stringStatus != FLECHA_RUNTIME_STRING_OK)
                status = FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
        }
    } catch (const std::bad_alloc&) {
        status = FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    } catch (...) {
        status = FLECHA_RUNTIME_FILESYSTEM_FAILED;
    }

    if (status != FLECHA_RUNTIME_FILESYSTEM_OK) {
        RemoveTemporaryDirectoryPath(path);
        flecha_runtime_v1_string_destroy(outputPath->data);
        *outputPath = {nullptr, 0, 0};
    }
    return status;
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
