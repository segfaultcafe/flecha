#include <cstddef>
#include <cstdint>
#include <string_view>

#include "runtime/runtime_abi.h"

namespace {

int successHandle;
int getFailureHandle;
std::size_t closeCount;

}  // namespace

extern "C" int32_t __wrap_flecha_runtime_v1_dns_resolve(void** output,
                                                        const char* name,
                                                        std::size_t length) {
    *output = nullptr;
    const std::string_view value(name, length);
    if (value == "out-of-memory") return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
    if (value == "invalid-name") return FLECHA_RUNTIME_NETWORK_INVALID_NAME;
    if (value == "name-not-found") return FLECHA_RUNTIME_NETWORK_NAME_NOT_FOUND;
    if (value == "temporary-failure")
        return FLECHA_RUNTIME_NETWORK_TEMPORARY_FAILURE;
    if (value == "unsupported-family")
        return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
    if (value == "resolution-failed")
        return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
    if (value == "success") {
        *output = &successHandle;
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    if (value == "get-failure") {
        *output = &getFailureHandle;
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
}

extern "C" std::size_t __wrap_flecha_runtime_v1_dns_result_count(
    const void* handle) {
    return handle == &successHandle || handle == &getFailureHandle ? 1 : 0;
}

extern "C" int32_t __wrap_flecha_runtime_v1_dns_result_get(const void* handle,
                                                           std::size_t index,
                                                           uint8_t* family,
                                                           uint64_t* high,
                                                           uint64_t* low) {
    *family = 0;
    *high = 0;
    *low = 0;
    if (handle == &getFailureHandle)
        return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
    if (handle != &successHandle || index != 0)
        return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
    *family = 4;
    *low = 0x7f000001;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" void __wrap_flecha_runtime_v1_dns_result_close(void* handle) {
    if (handle == &successHandle || handle == &getFailureHandle) ++closeCount;
}

extern "C" std::size_t flecha_test_dns_close_count() { return closeCount; }
