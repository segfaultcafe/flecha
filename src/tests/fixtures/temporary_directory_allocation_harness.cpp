#include <dirent.h>
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

}  // namespace

extern "C" void* __wrap_malloc(std::size_t size) {
    ++allocationIndex;
    if (allocationIndex == failedAllocationIndex) return nullptr;
    return __real_malloc(size);
}

int main() {
    char root[] = "/tmp/flecha-temporary-directory-allocation-XXXXXX";
    if (::mkdtemp(root) == nullptr) return 1;
    if (::setenv("TMPDIR", root, 1) != 0) return Cleanup(root, 2);

    allocationIndex = 0;
    failedAllocationIndex = 1;
    FlechaRuntimeString failedPath{reinterpret_cast<char*>(1), 1, 1};
    const int32_t failureStatus =
        flecha_runtime_v1_temporary_directory_create(&failedPath);
    failedAllocationIndex = 0;
    if (failureStatus != FLECHA_RUNTIME_FILESYSTEM_OUT_OF_MEMORY ||
        failedPath.data != nullptr || failedPath.length != 0 ||
        failedPath.capacity != 0 || !DirectoryIsEmpty(root))
        return Cleanup(root, 3);

    FlechaRuntimeString path{};
    if (flecha_runtime_v1_temporary_directory_create(&path) !=
            FLECHA_RUNTIME_FILESYSTEM_OK ||
        path.data == nullptr || path.length == 0)
        return Cleanup(root, 4);
    if (::rmdir(path.data) != 0) {
        flecha_runtime_v1_string_destroy(path.data);
        return Cleanup(root, 5);
    }
    flecha_runtime_v1_string_destroy(path.data);

    if (!DirectoryIsEmpty(root)) return Cleanup(root, 6);
    return Cleanup(root, 0);
}
