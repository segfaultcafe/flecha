#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
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
#include <dirent.h>
#include <fcntl.h>
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

struct DirectoryIterator final {
    HANDLE search;
    WIN32_FIND_DATAW pending;
    bool hasPending;
    bool exhausted;
};

int32_t MapWindowsFilesystemError(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_DIRECTORY:
            return FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return FLECHA_RUNTIME_FILESYSTEM_PERMISSION_DENIED;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return FLECHA_RUNTIME_FILESYSTEM_ALREADY_EXISTS;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_NO_UNICODE_TRANSLATION:
            return FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
        default:
            return FLECHA_RUNTIME_FILESYSTEM_FAILED;
    }
}
#else
using NativePathCharacter = char;

struct DirectoryIterator final {
    DIR* directory;
    dirent* pending;
    bool exhausted;
};
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

extern "C" uint8_t flecha_runtime_v1_path_style(void) {
#ifdef _WIN32
    return FLECHA_RUNTIME_PATH_STYLE_WINDOWS;
#else
    return FLECHA_RUNTIME_PATH_STYLE_POSIX;
#endif
}

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

extern "C" int32_t flecha_runtime_v1_directory_open(void** output,
                                                    const char* path,
                                                    size_t pathLength) {
    if (output == nullptr) return FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS;
    *output = nullptr;

    NativePathCharacter* nativePath = nullptr;
    const int32_t pathStatus = CopyNativePath(path, pathLength, &nativePath);
    if (pathStatus != FLECHA_RUNTIME_FILESYSTEM_OK) return pathStatus;

#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(nativePath);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const int32_t status = MapWindowsFilesystemError(GetLastError());
        std::free(nativePath);
        return status;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        std::free(nativePath);
        return FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND;
    }

    const size_t length = std::wcslen(nativePath);
    const bool hasTrailingSeparator =
        nativePath[length - 1] == L'\\' || nativePath[length - 1] == L'/';
    const bool isBareDrive = length == 2 && nativePath[1] == L':';
    const bool needsSeparator = !hasTrailingSeparator && !isBareDrive;
    const size_t additional = needsSeparator ? 3 : 2;
    if (length > std::numeric_limits<size_t>::max() - additional ||
        (length + additional) >
            std::numeric_limits<size_t>::max() / sizeof(wchar_t)) {
        std::free(nativePath);
        return FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE;
    }

    auto* pattern = static_cast<wchar_t*>(
        std::malloc((length + additional) * sizeof(wchar_t)));
    if (pattern == nullptr) {
        std::free(nativePath);
        return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    }
    std::memcpy(pattern, nativePath, length * sizeof(wchar_t));
    size_t patternLength = length;
    if (needsSeparator) pattern[patternLength++] = L'\\';
    pattern[patternLength++] = L'*';
    pattern[patternLength] = L'\0';

    auto* iterator =
        static_cast<DirectoryIterator*>(std::malloc(sizeof(DirectoryIterator)));
    if (iterator == nullptr) {
        std::free(pattern);
        std::free(nativePath);
        return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    }

    WIN32_FIND_DATAW first{};
    SetLastError(ERROR_SUCCESS);
    const HANDLE search = FindFirstFileW(pattern, &first);
    const DWORD findError = GetLastError();
    std::free(pattern);
    std::free(nativePath);

    if (search == INVALID_HANDLE_VALUE) {
        if (findError == ERROR_FILE_NOT_FOUND) {
            *iterator = {INVALID_HANDLE_VALUE, {}, false, true};
            *output = iterator;
            return FLECHA_RUNTIME_FILESYSTEM_OK;
        }
        std::free(iterator);
        return MapWindowsFilesystemError(findError);
    }

    *iterator = {search, first, true, false};
    *output = iterator;
    return FLECHA_RUNTIME_FILESYSTEM_OK;
#else
    errno = 0;
    DIR* directory = ::opendir(nativePath);
    const int openError = errno;
    std::free(nativePath);
    if (directory == nullptr) return MapFilesystemError(openError);

    auto* iterator =
        static_cast<DirectoryIterator*>(std::malloc(sizeof(DirectoryIterator)));
    if (iterator == nullptr) {
        (void)::closedir(directory);
        return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    }

    *iterator = {directory, nullptr, false};
    *output = iterator;
    return FLECHA_RUNTIME_FILESYSTEM_OK;
#endif
}

extern "C" int32_t flecha_runtime_v1_directory_next(
    void* handle, FlechaRuntimeString* outputName, uint8_t* outputType,
    uint8_t* outputHasEntry) {
    if (outputName != nullptr) *outputName = {nullptr, 0, 0};
    if (outputType != nullptr) *outputType = 0;
    if (outputHasEntry != nullptr) *outputHasEntry = 0;
    if (handle == nullptr || outputName == nullptr || outputType == nullptr ||
        outputHasEntry == nullptr)
        return FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS;

    auto* iterator = static_cast<DirectoryIterator*>(handle);

#ifdef _WIN32
    if (!iterator->hasPending) {
        if (iterator->exhausted) return FLECHA_RUNTIME_FILESYSTEM_OK;

        SetLastError(ERROR_SUCCESS);
        if (FindNextFileW(iterator->search, &iterator->pending) == 0) {
            const DWORD findError = GetLastError();
            if (findError == ERROR_NO_MORE_FILES) {
                iterator->exhausted = true;
                return FLECHA_RUNTIME_FILESYSTEM_OK;
            }
            return MapWindowsFilesystemError(findError);
        }
        iterator->hasPending = true;
    }

    const wchar_t* name = iterator->pending.cFileName;
    const size_t wideLength = std::wcslen(name);
    if (wideLength > static_cast<size_t>(INT_MAX)) {
        iterator->hasPending = false;
        return FLECHA_RUNTIME_FILESYSTEM_TOO_LARGE;
    }

    SetLastError(ERROR_SUCCESS);
    const int utf8Length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, name, static_cast<int>(wideLength),
        nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        iterator->hasPending = false;
        return MapWindowsFilesystemError(GetLastError());
    }

    auto* owned =
        static_cast<char*>(std::malloc(static_cast<size_t>(utf8Length) + 1));
    if (owned == nullptr) return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name,
                            static_cast<int>(wideLength), owned, utf8Length,
                            nullptr, nullptr) != utf8Length) {
        const DWORD conversionError = GetLastError();
        std::free(owned);
        iterator->hasPending = false;
        return MapWindowsFilesystemError(conversionError);
    }
    owned[utf8Length] = '\0';

    const DWORD attributes = iterator->pending.dwFileAttributes;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        *outputType = 3;
    else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        *outputType = 2;
    else
        *outputType = 1;

    *outputName = {owned, static_cast<size_t>(utf8Length),
                   static_cast<size_t>(utf8Length)};
    *outputHasEntry = 1;
    iterator->hasPending = false;
    return FLECHA_RUNTIME_FILESYSTEM_OK;
#else
    if (iterator->pending == nullptr) {
        if (iterator->exhausted) return FLECHA_RUNTIME_FILESYSTEM_OK;

        do {
            errno = 0;
            iterator->pending = ::readdir(iterator->directory);
        } while (iterator->pending == nullptr && errno == EINTR);
        if (iterator->pending == nullptr) {
            if (errno != 0) return MapFilesystemError(errno);
            iterator->exhausted = true;
            return FLECHA_RUNTIME_FILESYSTEM_OK;
        }
    }

    const int descriptor = ::dirfd(iterator->directory);
    if (descriptor < 0) {
        const int directoryError = errno;
        iterator->pending = nullptr;
        return MapFilesystemError(directoryError);
    }

    struct stat information{};
    int result = 0;
    do {
        errno = 0;
        result = ::fstatat(descriptor, iterator->pending->d_name, &information,
                           AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int metadataError = errno;
        iterator->pending = nullptr;
        return MapFilesystemError(metadataError);
    }

    uint8_t type = 0;
    if (S_ISREG(information.st_mode))
        type = 1;
    else if (S_ISDIR(information.st_mode))
        type = 2;
    else if (S_ISLNK(information.st_mode))
        type = 3;

    const size_t nameLength = std::strlen(iterator->pending->d_name);
    const int32_t stringStatus = flecha_runtime_v1_string_try_from_bytes(
        outputName, reinterpret_cast<const uint8_t*>(iterator->pending->d_name),
        nameLength);
    if (stringStatus != FLECHA_RUNTIME_STRING_OK)
        return FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY;

    *outputType = type;
    *outputHasEntry = 1;
    iterator->pending = nullptr;
    return FLECHA_RUNTIME_FILESYSTEM_OK;
#endif
}

extern "C" int32_t flecha_runtime_v1_directory_close(void* handle) {
    if (handle == nullptr) return FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS;

    auto* iterator = static_cast<DirectoryIterator*>(handle);

#ifdef _WIN32
    BOOL result = TRUE;
    DWORD closeError = ERROR_SUCCESS;
    if (iterator->search != INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_SUCCESS);
        result = FindClose(iterator->search);
        closeError = GetLastError();
    }
    std::free(iterator);
    return result != 0 ? FLECHA_RUNTIME_FILESYSTEM_OK
                       : MapWindowsFilesystemError(closeError);
#else
    errno = 0;
    const int result = ::closedir(iterator->directory);
    const int closeError = errno;
    std::free(iterator);
    return result == 0 ? FLECHA_RUNTIME_FILESYSTEM_OK
                       : MapFilesystemError(closeError);
#endif
}
