#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

int32_t MapOpenError(int error) {
    switch (error) {
        case ENOENT:
#if defined(ENOTDIR) && ENOTDIR != ENOENT
        case ENOTDIR:
#endif
            return FLECHA_RUNTIME_READ_FILE_NOT_FOUND;
        case EACCES:
#if defined(EPERM) && EPERM != EACCES
        case EPERM:
#endif
            return FLECHA_RUNTIME_READ_FILE_PERMISSION_DENIED;
#if defined(ENAMETOOLONG) && ENAMETOOLONG != EINVAL
        case ENAMETOOLONG:
#endif
        case EINVAL:
            return FLECHA_RUNTIME_READ_FILE_INVALID_PATH;
        default:
            return FLECHA_RUNTIME_READ_FILE_FAILED;
    }
}

int32_t MapFilesystemError(int error) {
    switch (error) {
        case ENOENT:
#if defined(ENOTDIR) && ENOTDIR != ENOENT
        case ENOTDIR:
#endif
            return FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND;
        case EACCES:
#if defined(EPERM) && EPERM != EACCES
        case EPERM:
#endif
#if defined(EROFS) && EROFS != EACCES && EROFS != EPERM
        case EROFS:
#endif
            return FLECHA_RUNTIME_FILESYSTEM_PERMISSION_DENIED;
        case EEXIST:
            return FLECHA_RUNTIME_FILESYSTEM_ALREADY_EXISTS;
#if defined(ENAMETOOLONG) && ENAMETOOLONG != EINVAL
        case ENAMETOOLONG:
#endif
        case EINVAL:
            return FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH;
#if defined(EFBIG)
        case EFBIG:
#endif
#if defined(EOVERFLOW) && EOVERFLOW != EFBIG
        case EOVERFLOW:
#endif
            return FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE;
#if defined(ENOMEM)
        case ENOMEM:
            return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
#endif
        default:
            return FLECHA_RUNTIME_FILESYSTEM_FAILED;
    }
}

bool IsMissingError(int error) {
    if (error == ENOENT) return true;
#if defined(ENOTDIR) && ENOTDIR != ENOENT
    if (error == ENOTDIR) return true;
#endif
    return false;
}

bool IsDirectoryNotEmptyError(int error) {
#if defined(ENOTEMPTY)
    if (error == ENOTEMPTY) return true;
#endif
#if defined(EEXIST)
    if (error == EEXIST) return true;
#endif
    return false;
}

int32_t MapFilesystemError(const std::error_code& error) {
    const std::error_condition condition = error.default_error_condition();
    if (condition == std::errc::no_such_file_or_directory ||
        condition == std::errc::not_a_directory)
        return FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND;
    if (condition == std::errc::permission_denied ||
        condition == std::errc::read_only_file_system)
        return FLECHA_RUNTIME_FILESYSTEM_PERMISSION_DENIED;
    if (condition == std::errc::file_exists)
        return FLECHA_RUNTIME_FILESYSTEM_ALREADY_EXISTS;
    if (condition == std::errc::invalid_argument ||
        condition == std::errc::filename_too_long)
        return FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH;
    if (condition == std::errc::file_too_large ||
        condition == std::errc::value_too_large)
        return FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE;
    if (condition == std::errc::not_enough_memory)
        return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    if (condition == std::errc::directory_not_empty)
        return FLECHA_RUNTIME_FILESYSTEM_DIRECTORY_NOT_EMPTY;
    return FLECHA_RUNTIME_FILESYSTEM_FAILED;
}

#ifdef _WIN32
using NativePathCharacter = wchar_t;
#else
using NativePathCharacter = char;
#endif

int32_t CopyNativePath(const char* path, size_t pathLength,
                       NativePathCharacter** output) {
    *output = nullptr;
    if (path == nullptr || pathLength == 0 ||
        std::memchr(path, '\0', pathLength) != nullptr)
        return FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH;

#ifdef _WIN32
    if (pathLength > static_cast<size_t>(INT_MAX))
        return FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH;
    const int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            static_cast<int>(pathLength), nullptr, 0);
    if (wideLength <= 0) return FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH;
    const size_t characters = static_cast<size_t>(wideLength) + 1;
    if (characters >
        std::numeric_limits<size_t>::max() / sizeof(NativePathCharacter))
        return FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE;
    auto* copied = static_cast<NativePathCharacter*>(
        std::malloc(characters * sizeof(NativePathCharacter)));
    if (copied == nullptr) return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            static_cast<int>(pathLength), copied,
                            wideLength) != wideLength) {
        std::free(copied);
        return FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH;
    }
    copied[wideLength] = L'\0';
#else
    if (pathLength == std::numeric_limits<size_t>::max())
        return FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE;
    auto* copied =
        static_cast<NativePathCharacter*>(std::malloc(pathLength + 1));
    if (copied == nullptr) return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    std::memcpy(copied, path, pathLength);
    copied[pathLength] = '\0';
#endif

    *output = copied;
    return FLECHA_RUNTIME_FILESYSTEM_OK;
}

FILE* OpenBinary(const char* path, size_t path_length, int32_t* status) {
#ifdef _WIN32
    if (path_length > static_cast<size_t>(INT_MAX)) {
        *status = FLECHA_RUNTIME_READ_FILE_INVALID_PATH;
        return nullptr;
    }
    const int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            static_cast<int>(path_length), nullptr, 0);
    if (wideLength <= 0) {
        *status = FLECHA_RUNTIME_READ_FILE_INVALID_PATH;
        return nullptr;
    }
    auto* widePath = static_cast<wchar_t*>(
        std::malloc((static_cast<size_t>(wideLength) + 1) * sizeof(wchar_t)));
    if (widePath == nullptr) {
        *status = FLECHA_RUNTIME_READ_FILE_OUT_OF_MEMORY;
        return nullptr;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            static_cast<int>(path_length), widePath,
                            wideLength) != wideLength) {
        std::free(widePath);
        *status = FLECHA_RUNTIME_READ_FILE_INVALID_PATH;
        return nullptr;
    }
    widePath[wideLength] = L'\0';
    errno = 0;
    FILE* file = _wfopen(widePath, L"rb");
    const int openError = errno;
    std::free(widePath);
    if (file == nullptr) *status = MapOpenError(openError);
    return file;
#else
    if (path_length == std::numeric_limits<size_t>::max()) {
        *status = FLECHA_RUNTIME_READ_FILE_INVALID_PATH;
        return nullptr;
    }
    auto* terminated = static_cast<char*>(std::malloc(path_length + 1));
    if (terminated == nullptr) {
        *status = FLECHA_RUNTIME_READ_FILE_OUT_OF_MEMORY;
        return nullptr;
    }
    std::memcpy(terminated, path, path_length);
    terminated[path_length] = '\0';
    errno = 0;
    FILE* file = std::fopen(terminated, "rb");
    const int openError = errno;
    std::free(terminated);
    if (file == nullptr) *status = MapOpenError(openError);
    return file;
#endif
}

}  // namespace

extern "C" int32_t flecha_runtime_v1_read_file(FlechaRuntimeString* output,
                                               const char* path,
                                               size_t path_length) {
    if (output == nullptr) return FLECHA_RUNTIME_READ_FILE_INVALID_PATH;
    *output = {nullptr, 0, 0};
    if (path == nullptr || path_length == 0 ||
        std::memchr(path, '\0', path_length) != nullptr)
        return FLECHA_RUNTIME_READ_FILE_INVALID_PATH;

    int32_t status = FLECHA_RUNTIME_READ_FILE_OK;
    FILE* file = OpenBinary(path, path_length, &status);
    if (file == nullptr) return status;

    constexpr size_t CHUNK_SIZE = 64 * 1024;
    constexpr size_t MAX_LENGTH =
        std::numeric_limits<size_t>::max() - 1 < UINT32_MAX
            ? std::numeric_limits<size_t>::max() - 1
            : UINT32_MAX;
    unsigned char chunk[CHUNK_SIZE];
    char* data = nullptr;
    size_t length = 0;
    size_t capacity = 0;
    while (true) {
        errno = 0;
        const size_t count = std::fread(chunk, 1, CHUNK_SIZE, file);
        if (count != 0) {
            if (count > MAX_LENGTH - length) {
                status = FLECHA_RUNTIME_READ_FILE_TOO_LARGE;
                break;
            }
            const size_t required = length + count;
            if (required > capacity) {
                size_t grown = capacity == 0 ? CHUNK_SIZE : capacity;
                while (grown < required) {
                    if (grown > MAX_LENGTH / 2) {
                        grown = MAX_LENGTH;
                        break;
                    }
                    grown *= 2;
                }
                char* replacement =
                    static_cast<char*>(std::realloc(data, grown + 1));
                if (replacement == nullptr) {
                    status = FLECHA_RUNTIME_READ_FILE_OUT_OF_MEMORY;
                    break;
                }
                data = replacement;
                capacity = grown;
            }
            std::memcpy(data + length, chunk, count);
            length = required;
        }
        if (count != CHUNK_SIZE) {
            if (std::ferror(file) != 0 && errno == EINTR) {
                std::clearerr(file);
                continue;
            }
            if (std::ferror(file) != 0)
                status = FLECHA_RUNTIME_READ_FILE_FAILED;
            break;
        }
    }

    if (std::fclose(file) != 0 && status == FLECHA_RUNTIME_READ_FILE_OK)
        status = FLECHA_RUNTIME_READ_FILE_FAILED;
    if (status != FLECHA_RUNTIME_READ_FILE_OK) {
        std::free(data);
        return status;
    }
    if (data == nullptr) {
        data = static_cast<char*>(std::malloc(1));
        if (data == nullptr) return FLECHA_RUNTIME_READ_FILE_OUT_OF_MEMORY;
    }
    data[length] = '\0';
    *output = {data, length, capacity};
    return FLECHA_RUNTIME_READ_FILE_OK;
}

extern "C" int32_t flecha_runtime_v1_path_exists(uint8_t* output,
                                                 const char* path,
                                                 size_t pathLength) {
    if (output == nullptr) return FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS;
    *output = 0;

    NativePathCharacter* nativePath = nullptr;
    const int32_t pathStatus = CopyNativePath(path, pathLength, &nativePath);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) return pathStatus;

    errno = 0;
#ifdef _WIN32
    struct _stat64 information{};
    const int result = _wstat64(nativePath, &information);
#else
    struct stat information{};
    const int result = ::stat(nativePath, &information);
#endif
    const int operationError = errno;
    std::free(nativePath);
    if (result == 0) {
        *output = 1;
        return FLECHA_RUNTIME_FILESYSTEM_OK;
    }
    return IsMissingError(operationError) ? FLECHA_RUNTIME_FILESYSTEM_OK
                                          : MapFilesystemError(operationError);
}

extern "C" int32_t flecha_runtime_v1_metadata(uint8_t* outputType,
                                              uint64_t* outputSize,
                                              const char* path,
                                              size_t pathLength) {
    if (outputType == nullptr || outputSize == nullptr)
        return FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS;
    *outputType = 0;
    *outputSize = 0;

    NativePathCharacter* nativePath = nullptr;
    const int32_t pathStatus = CopyNativePath(path, pathLength, &nativePath);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) return pathStatus;

    int result = 0;
#ifdef _WIN32
    struct _stat64 information{};
    do {
        errno = 0;
        result = _wstat64(nativePath, &information);
    } while (result != 0 && errno == EINTR);
#else
    struct stat information{};
    do {
        errno = 0;
        result = ::stat(nativePath, &information);
    } while (result != 0 && errno == EINTR);
#endif
    const int operationError = errno;
    std::free(nativePath);
    if (result != 0) return MapFilesystemError(operationError);

#ifdef _WIN32
    const int entryType = information.st_mode & _S_IFMT;
    const bool isFile = entryType == _S_IFREG;
    const bool isDirectory = entryType == _S_IFDIR;
#else
    const bool isFile = S_ISREG(information.st_mode);
    const bool isDirectory = S_ISDIR(information.st_mode);
#endif

    if (isFile) {
        if (information.st_size < 0) return FLECHA_RUNTIME_FILESYSTEM_FAILED;
        if (!std::in_range<uint64_t>(information.st_size))
            return FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE;
        *outputType = 1;
        *outputSize = static_cast<uint64_t>(information.st_size);
    } else if (isDirectory) {
        *outputType = 2;
    }
    return FLECHA_RUNTIME_FILESYSTEM_OK;
}

extern "C" int32_t flecha_runtime_v1_remove_file(const char* path,
                                                 size_t pathLength) {
    NativePathCharacter* nativePath = nullptr;
    const int32_t pathStatus = CopyNativePath(path, pathLength, &nativePath);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) return pathStatus;

    errno = 0;
#ifdef _WIN32
    const int result = _wunlink(nativePath);
#else
    const int result = ::unlink(nativePath);
#endif
    const int operationError = errno;
    std::free(nativePath);
    return result == 0 ? FLECHA_RUNTIME_FILESYSTEM_OK
                       : MapFilesystemError(operationError);
}

extern "C" int32_t flecha_runtime_v1_rename(const char* source,
                                            size_t sourceLength,
                                            const char* destination,
                                            size_t destinationLength) {
    NativePathCharacter* nativeSource = nullptr;
    int32_t pathStatus = CopyNativePath(source, sourceLength, &nativeSource);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) return pathStatus;

    NativePathCharacter* nativeDestination = nullptr;
    pathStatus =
        CopyNativePath(destination, destinationLength, &nativeDestination);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) {
        std::free(nativeSource);
        return pathStatus;
    }

    errno = 0;
#ifdef _WIN32
    const int result = _wrename(nativeSource, nativeDestination);
#else
    const int result = ::rename(nativeSource, nativeDestination);
#endif
    const int operationError = errno;
    std::free(nativeDestination);
    std::free(nativeSource);
    return result == 0 ? FLECHA_RUNTIME_FILESYSTEM_OK
                       : MapFilesystemError(operationError);
}

extern "C" int32_t flecha_runtime_v1_create_directory(const char* path,
                                                      size_t pathLength) {
    NativePathCharacter* nativePath = nullptr;
    const int32_t pathStatus = CopyNativePath(path, pathLength, &nativePath);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) return pathStatus;

    errno = 0;
#ifdef _WIN32
    const int result = _wmkdir(nativePath);
#else
    const int result = ::mkdir(nativePath, 0777);
#endif
    const int operationError = errno;
    std::free(nativePath);
    return result == 0 ? FLECHA_RUNTIME_FILESYSTEM_OK
                       : MapFilesystemError(operationError);
}

extern "C" int32_t flecha_runtime_v1_remove_directory(const char* path,
                                                      size_t pathLength,
                                                      uint8_t force,
                                                      uint8_t recursive) {
    if (force > 1 || recursive > 1)
        return FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS;

    NativePathCharacter* nativePath = nullptr;
    const int32_t pathStatus = CopyNativePath(path, pathLength, &nativePath);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) return pathStatus;

    if (recursive == 0) {
        errno = 0;
#ifdef _WIN32
        const int result = _wrmdir(nativePath);
#else
        const int result = ::rmdir(nativePath);
#endif
        const int operationError = errno;
        std::free(nativePath);
        if (result == 0) return FLECHA_RUNTIME_FILESYSTEM_OK;
        if (force != 0 && IsMissingError(operationError))
            return FLECHA_RUNTIME_FILESYSTEM_OK;
        if (IsDirectoryNotEmptyError(operationError))
            return FLECHA_RUNTIME_FILESYSTEM_DIRECTORY_NOT_EMPTY;
        return MapFilesystemError(operationError);
    }

    int32_t resultStatus = FLECHA_RUNTIME_FILESYSTEM_FAILED;
    try {
        const std::filesystem::path directory(nativePath);
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(directory, error);
        if (error) {
            resultStatus =
                force != 0 && error.default_error_condition() ==
                                  std::errc::no_such_file_or_directory
                    ? FLECHA_RUNTIME_FILESYSTEM_OK
                    : MapFilesystemError(error);
        } else if (status.type() == std::filesystem::file_type::not_found) {
            resultStatus = force != 0 ? FLECHA_RUNTIME_FILESYSTEM_OK
                                      : FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND;
        } else if (!std::filesystem::is_directory(status)) {
            resultStatus = FLECHA_RUNTIME_FILESYSTEM_FAILED;
        } else {
            (void)std::filesystem::remove_all(directory, error);
            if (!error)
                resultStatus = FLECHA_RUNTIME_FILESYSTEM_OK;
            else if (force != 0 && error.default_error_condition() ==
                                       std::errc::no_such_file_or_directory)
                resultStatus = FLECHA_RUNTIME_FILESYSTEM_OK;
            else
                resultStatus = MapFilesystemError(error);
        }
    } catch (const std::bad_alloc&) {
        resultStatus = FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    } catch (...) {
        resultStatus = FLECHA_RUNTIME_FILESYSTEM_FAILED;
    }
    std::free(nativePath);
    return resultStatus;
}
