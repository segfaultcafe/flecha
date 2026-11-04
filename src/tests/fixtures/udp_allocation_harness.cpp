#include <dirent.h>

#include <cstddef>
#include <cstdint>
#include <new>

#include "runtime/runtime_abi.h"

namespace {

bool failNextAllocation = false;

std::size_t OpenDescriptorCount() {
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr) return 0;
    std::size_t count = 0;
    while (readdir(directory) != nullptr) ++count;
    closedir(directory);
    return count;
}

}  // namespace

extern "C" void* __real__Znwm(std::size_t size);

extern "C" void* __wrap__Znwm(std::size_t size) {
    if (failNextAllocation) {
        failNextAllocation = false;
        throw std::bad_alloc();
    }
    return __real__Znwm(size);
}

int main() {
    constexpr uint64_t loopback = UINT64_C(0x7f000001);
    const std::size_t descriptorsBefore = OpenDescriptorCount();
    if (descriptorsBefore == 0) return 1;

    void* socket = reinterpret_cast<void*>(1);
    failNextAllocation = true;
    const int32_t status =
        flecha_runtime_v1_udp_socket_bind(&socket, 4, 0, loopback, 0);
    if (status != FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY || socket != nullptr ||
        failNextAllocation)
        return 2;

    const std::size_t descriptorsAfter = OpenDescriptorCount();
    if (descriptorsAfter == 0 || descriptorsAfter > descriptorsBefore)
        return 3;

    if (flecha_runtime_v1_udp_socket_bind(&socket, 4, 0, loopback, 0) !=
            FLECHA_RUNTIME_NETWORK_OK ||
        socket == nullptr)
        return 4;
    if (flecha_runtime_v1_udp_socket_close(socket) != FLECHA_RUNTIME_NETWORK_OK)
        return 5;
    return 0;
}
