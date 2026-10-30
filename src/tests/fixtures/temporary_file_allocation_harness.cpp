#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "runtime/runtime_abi.h"

extern "C" void* __real_malloc(std::size_t size);

namespace {

std::size_t allocationIndex = 0;
std::size_t failedAllocationIndex = 0;

bool DirectoryIsEmpty(const char* directory) {
    DIR* entries = ::opendir(directory);
    if (entries == nullptr) return false;

    bool empty = true;
    while (dirent* entry = ::readdir(entries)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    (void)::closedir(entries);
    return empty;
}

int Cleanup(const char* directory, int result) {
    (void)::rmdir(directory);
    return result;
}

bool FailsCleanlyAt(const char* directory, std::size_t failure) {
    allocationIndex = 0;
    failedAllocationIndex = failure;

    void* handle = reinterpret_cast<void*>(1);
    FlechaRuntimeString path{reinterpret_cast<char*>(1), 1, 1};
    const int32_t status =
        flecha_runtime_v1_temporary_file_open(&handle, &path);
    failedAllocationIndex = 0;

    return status == FLECHA_RUNTIME_FILE_STREAM_OUT_OF_MEMORY &&
           handle == nullptr && path.data == nullptr && path.length == 0 &&
           path.capacity == 0 && DirectoryIsEmpty(directory);
}

}  // namespace

extern "C" void* __wrap_malloc(std::size_t size) {
    ++allocationIndex;
    if (allocationIndex == failedAllocationIndex) return nullptr;
    return __real_malloc(size);
}

int main() {
    char directory[] = "/tmp/flecha-temporary-allocation-XXXXXX";
    if (::mkdtemp(directory) == nullptr) return 1;
    if (::setenv("TMPDIR", directory, 1) != 0) return Cleanup(directory, 2);

    // The first runtime allocation owns the stream handle; the second owns the
    // returned Flecha path string. Both failures must remove the OS file.
    if (!FailsCleanlyAt(directory, 1)) return Cleanup(directory, 3);
    if (!FailsCleanlyAt(directory, 2)) return Cleanup(directory, 4);

    void* handle = nullptr;
    FlechaRuntimeString path{};
    if (flecha_runtime_v1_temporary_file_open(&handle, &path) !=
            FLECHA_RUNTIME_FILE_STREAM_OK ||
        handle == nullptr || path.data == nullptr || path.length == 0)
        return Cleanup(directory, 5);

    if (flecha_runtime_v1_file_stream_close(handle) !=
        FLECHA_RUNTIME_FILE_STREAM_OK) {
        flecha_runtime_v1_string_destroy(path.data);
        return Cleanup(directory, 6);
    }
    if (::unlink(path.data) != 0) {
        flecha_runtime_v1_string_destroy(path.data);
        return Cleanup(directory, 7);
    }
    flecha_runtime_v1_string_destroy(path.data);

    if (!DirectoryIsEmpty(directory)) return Cleanup(directory, 8);
    return Cleanup(directory, 0);
}
