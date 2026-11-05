#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

bool ValidEnvironmentName(const char* name, size_t length) {
    return name != nullptr && length != 0 &&
           std::memchr(name, '\0', length) == nullptr &&
           std::memchr(name, '=', length) == nullptr;
}

bool ValidEnvironmentValue(const char* value, size_t length) {
    return (value != nullptr || length == 0) &&
           (length == 0 || std::memchr(value, '\0', length) == nullptr);
}

#ifdef _WIN32

int32_t MapWindowsEnvironmentError(DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED:
            return FLECHA_RUNTIME_ENVIRONMENT_PERMISSION_DENIED;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
        default:
            return FLECHA_RUNTIME_ENVIRONMENT_FAILED;
    }
}

int32_t CopyWide(const char* source, size_t length, wchar_t** output) {
    *output = nullptr;
    if (length > static_cast<size_t>(INT_MAX))
        return FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE;

    if (length == 0) {
        auto* empty = static_cast<wchar_t*>(std::malloc(sizeof(wchar_t)));
        if (empty == nullptr) return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
        empty[0] = L'\0';
        *output = empty;
        return FLECHA_RUNTIME_ENVIRONMENT_OK;
    }

    const int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source,
                            static_cast<int>(length), nullptr, 0);
    if (wideLength <= 0) return FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE;
    if (static_cast<size_t>(wideLength) >
        std::numeric_limits<size_t>::max() / sizeof(wchar_t) - 1)
        return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;

    auto* copied = static_cast<wchar_t*>(
        std::malloc((static_cast<size_t>(wideLength) + 1) * sizeof(wchar_t)));
    if (copied == nullptr) return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source,
                            static_cast<int>(length), copied,
                            wideLength) != wideLength) {
        std::free(copied);
        return FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE;
    }
    copied[wideLength] = L'\0';
    *output = copied;
    return FLECHA_RUNTIME_ENVIRONMENT_OK;
}

int32_t CopyWideValueToString(FlechaRuntimeString* output, const wchar_t* value,
                              size_t length) {
    if (length == 0) return FLECHA_RUNTIME_ENVIRONMENT_OK;
    if (length > static_cast<size_t>(INT_MAX))
        return FLECHA_RUNTIME_ENVIRONMENT_FAILED;

    const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                               value, static_cast<int>(length),
                                               nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return FLECHA_RUNTIME_ENVIRONMENT_FAILED;

    auto* utf8 =
        static_cast<char*>(std::malloc(static_cast<size_t>(utf8Length)));
    if (utf8 == nullptr) return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                            static_cast<int>(length), utf8, utf8Length, nullptr,
                            nullptr) != utf8Length) {
        std::free(utf8);
        return FLECHA_RUNTIME_ENVIRONMENT_FAILED;
    }

    const int32_t stringStatus = flecha_runtime_v1_string_try_from_bytes(
        output, reinterpret_cast<const uint8_t*>(utf8),
        static_cast<size_t>(utf8Length));
    std::free(utf8);
    return stringStatus == FLECHA_RUNTIME_STRING_OK
               ? FLECHA_RUNTIME_ENVIRONMENT_OK
               : FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
}

#else

int32_t CopyBytes(const char* source, size_t length, char** output) {
    *output = nullptr;
    if (length == std::numeric_limits<size_t>::max())
        return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
    auto* copied = static_cast<char*>(std::malloc(length + 1));
    if (copied == nullptr) return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
    if (length != 0) std::memcpy(copied, source, length);
    copied[length] = '\0';
    *output = copied;
    return FLECHA_RUNTIME_ENVIRONMENT_OK;
}

int32_t MapPosixEnvironmentError(int error) {
    switch (error) {
        case EACCES:
            return FLECHA_RUNTIME_ENVIRONMENT_PERMISSION_DENIED;
        case ENOMEM:
            return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
        default:
            return FLECHA_RUNTIME_ENVIRONMENT_FAILED;
    }
}

#endif

}  // namespace

extern "C" {

int32_t flecha_runtime_v1_environment_get(FlechaRuntimeString* output,
                                          uint8_t* present, const char* name,
                                          size_t nameLength) {
    if (output == nullptr || present == nullptr)
        return FLECHA_RUNTIME_ENVIRONMENT_FAILED;
    *output = {nullptr, 0, 0};
    *present = 0;
    if (!ValidEnvironmentName(name, nameLength))
        return FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME;

#ifdef _WIN32
    wchar_t* nativeName = nullptr;
    int32_t status = CopyWide(name, nameLength, &nativeName);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK) {
        return status == FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE
                   ? FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME
                   : status;
    }

    SetLastError(ERROR_SUCCESS);
    DWORD capacity = GetEnvironmentVariableW(nativeName, nullptr, 0);
    if (capacity == 0) {
        const DWORD error = GetLastError();
        std::free(nativeName);
        if (error == ERROR_ENVVAR_NOT_FOUND)
            return FLECHA_RUNTIME_ENVIRONMENT_OK;
        if (error == ERROR_SUCCESS) {
            *present = 1;
            return FLECHA_RUNTIME_ENVIRONMENT_OK;
        }
        return MapWindowsEnvironmentError(error);
    }

    for (;;) {
        if (static_cast<size_t>(capacity) >
            std::numeric_limits<size_t>::max() / sizeof(wchar_t)) {
            std::free(nativeName);
            return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
        }
        auto* value = static_cast<wchar_t*>(
            std::malloc(static_cast<size_t>(capacity) * sizeof(wchar_t)));
        if (value == nullptr) {
            std::free(nativeName);
            return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
        }

        SetLastError(ERROR_SUCCESS);
        const DWORD length =
            GetEnvironmentVariableW(nativeName, value, capacity);
        if (length < capacity) {
            const DWORD error = GetLastError();
            if (length == 0 && error == ERROR_ENVVAR_NOT_FOUND) {
                std::free(value);
                std::free(nativeName);
                return FLECHA_RUNTIME_ENVIRONMENT_OK;
            }
            if (length == 0 && error != ERROR_SUCCESS) {
                std::free(value);
                std::free(nativeName);
                return MapWindowsEnvironmentError(error);
            }
            status = CopyWideValueToString(output, value, length);
            std::free(value);
            std::free(nativeName);
            if (status == FLECHA_RUNTIME_ENVIRONMENT_OK) *present = 1;
            return status;
        }

        std::free(value);
        capacity = length + 1;
        if (capacity == 0) {
            std::free(nativeName);
            return FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
        }
    }
#else
    char* nativeName = nullptr;
    int32_t status = CopyBytes(name, nameLength, &nativeName);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK) return status;

    const char* value = std::getenv(nativeName);
    if (value == nullptr) {
        std::free(nativeName);
        return FLECHA_RUNTIME_ENVIRONMENT_OK;
    }
    const size_t valueLength = std::strlen(value);
    status = flecha_runtime_v1_string_try_from_bytes(
                 output, reinterpret_cast<const uint8_t*>(value),
                 valueLength) == FLECHA_RUNTIME_STRING_OK
                 ? FLECHA_RUNTIME_ENVIRONMENT_OK
                 : FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY;
    std::free(nativeName);
    if (status == FLECHA_RUNTIME_ENVIRONMENT_OK) *present = 1;
    return status;
#endif
}

int32_t flecha_runtime_v1_environment_set(const char* name, size_t nameLength,
                                          const char* value,
                                          size_t valueLength) {
    if (!ValidEnvironmentName(name, nameLength))
        return FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME;
    if (!ValidEnvironmentValue(value, valueLength))
        return FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE;

#ifdef _WIN32
    wchar_t* nativeName = nullptr;
    int32_t status = CopyWide(name, nameLength, &nativeName);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK)
        return status == FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE
                   ? FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME
                   : status;
    wchar_t* nativeValue = nullptr;
    status = CopyWide(value, valueLength, &nativeValue);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK) {
        std::free(nativeName);
        return status;
    }
    const BOOL succeeded = SetEnvironmentVariableW(nativeName, nativeValue);
    const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
    std::free(nativeValue);
    std::free(nativeName);
    return succeeded ? FLECHA_RUNTIME_ENVIRONMENT_OK
                     : MapWindowsEnvironmentError(error);
#else
    char* nativeName = nullptr;
    int32_t status = CopyBytes(name, nameLength, &nativeName);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK) return status;
    char* nativeValue = nullptr;
    status = CopyBytes(value, valueLength, &nativeValue);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK) {
        std::free(nativeName);
        return status;
    }
    errno = 0;
    const int result = ::setenv(nativeName, nativeValue, 1);
    const int error = errno;
    std::free(nativeValue);
    std::free(nativeName);
    return result == 0 ? FLECHA_RUNTIME_ENVIRONMENT_OK
                       : MapPosixEnvironmentError(error);
#endif
}

int32_t flecha_runtime_v1_environment_remove(const char* name,
                                             size_t nameLength) {
    if (!ValidEnvironmentName(name, nameLength))
        return FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME;

#ifdef _WIN32
    wchar_t* nativeName = nullptr;
    int32_t status = CopyWide(name, nameLength, &nativeName);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK)
        return status == FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE
                   ? FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME
                   : status;
    const BOOL succeeded = SetEnvironmentVariableW(nativeName, nullptr);
    const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
    std::free(nativeName);
    if (succeeded || error == ERROR_ENVVAR_NOT_FOUND)
        return FLECHA_RUNTIME_ENVIRONMENT_OK;
    return MapWindowsEnvironmentError(error);
#else
    char* nativeName = nullptr;
    int32_t status = CopyBytes(name, nameLength, &nativeName);
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OK) return status;
    errno = 0;
    const int result = ::unsetenv(nativeName);
    const int error = errno;
    std::free(nativeName);
    return result == 0 ? FLECHA_RUNTIME_ENVIRONMENT_OK
                       : MapPosixEnvironmentError(error);
#endif
}

}  // extern "C"
