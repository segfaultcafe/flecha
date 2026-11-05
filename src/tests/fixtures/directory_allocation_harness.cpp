#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "runtime/runtime_abi.h"

extern "C" void* __real_malloc(std::size_t size);

namespace {

std::size_t failedAllocationSize = 0;

int Cleanup(const char* directory, const char* file, int result) {
    (void)::unlink(file);
    (void)::rmdir(directory);
    return result;
}

}  // namespace

extern "C" void* __wrap_malloc(std::size_t size) {
    if (size == failedAllocationSize) {
        failedAllocationSize = 0;
        return nullptr;
    }
    return __real_malloc(size);
}

int main() {
    char directory[] = "/tmp/flecha-directory-allocation-XXXXXX";
    if (::mkdtemp(directory) == nullptr) return 1;

    char file[sizeof(directory) + 7]{};
    std::memcpy(file, directory, sizeof(directory) - 1);
    std::memcpy(file + sizeof(directory) - 1, "/entry", 7);
    const int descriptor = ::open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return Cleanup(directory, file, 2);
    if (::close(descriptor) != 0) return Cleanup(directory, file, 3);

    void* handle = nullptr;
    if (flecha_runtime_v1_directory_open(&handle, directory,
                                         sizeof(directory) - 1) !=
            FLECHA_RUNTIME_FILESYSTEM_OK ||
        handle == nullptr)
        return Cleanup(directory, file, 4);

    failedAllocationSize = 6;
    FlechaRuntimeString name{};
    uint8_t type = 0;
    uint8_t hasEntry = 0;
    while (true) {
        const int32_t status =
            flecha_runtime_v1_directory_next(handle, &name, &type, &hasEntry);
        if (status == FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY) break;
        if (status != FLECHA_RUNTIME_FILESYSTEM_OK || hasEntry == 0) {
            (void)flecha_runtime_v1_directory_close(handle);
            return Cleanup(directory, file, 5);
        }
        flecha_runtime_v1_string_destroy(name.data);
        name = {};
    }

    if (failedAllocationSize != 0 || name.data != nullptr || type != 0 ||
        hasEntry != 0) {
        (void)flecha_runtime_v1_directory_close(handle);
        return Cleanup(directory, file, 6);
    }

    if (flecha_runtime_v1_directory_next(handle, &name, &type, &hasEntry) !=
            FLECHA_RUNTIME_FILESYSTEM_OK ||
        hasEntry != 1 || type != 1 || name.length != 5 ||
        std::memcmp(name.data, "entry", 5) != 0) {
        (void)flecha_runtime_v1_directory_close(handle);
        return Cleanup(directory, file, 7);
    }
    flecha_runtime_v1_string_destroy(name.data);
    name = {};

    while (true) {
        if (flecha_runtime_v1_directory_next(handle, &name, &type, &hasEntry) !=
            FLECHA_RUNTIME_FILESYSTEM_OK) {
            (void)flecha_runtime_v1_directory_close(handle);
            return Cleanup(directory, file, 8);
        }
        if (hasEntry == 0) break;

        if (name.length == 5 && std::memcmp(name.data, "entry", 5) == 0) {
            flecha_runtime_v1_string_destroy(name.data);
            (void)flecha_runtime_v1_directory_close(handle);
            return Cleanup(directory, file, 9);
        }
        flecha_runtime_v1_string_destroy(name.data);
        name = {};
    }

    if (flecha_runtime_v1_directory_close(handle) !=
        FLECHA_RUNTIME_FILESYSTEM_OK)
        return Cleanup(directory, file, 10);
    return Cleanup(directory, file, 0);
}
