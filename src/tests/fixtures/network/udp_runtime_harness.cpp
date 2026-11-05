#include <cstddef>
#include <cstdint>

#include "runtime/runtime_abi.h"

namespace {
int socketHandle;
int closeFailureHandle;
int automaticHandle;
std::size_t closeCount;
unsigned sendCalls;
unsigned receiveCalls;
uint8_t timeoutMask;

void ResetAddress(uint8_t* family, uint64_t* high, uint64_t* low,
                  uint16_t* port) {
    if (family != nullptr) *family = 0;
    if (high != nullptr) *high = 0;
    if (low != nullptr) *low = 0;
    if (port != nullptr) *port = 0;
}
}  // namespace

extern "C" int32_t __wrap_flecha_runtime_v1_udp_socket_bind(
    void** output, uint8_t family, uint64_t high, uint64_t low, uint16_t port) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = nullptr;
    if (port == 10001) return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
    if (port == 10012) return FLECHA_RUNTIME_NETWORK_ADDRESS_IN_USE;
    if (port == 10015) return FLECHA_RUNTIME_NETWORK_FAILED;
    if (family != 4 || high != 0 || low != UINT64_C(0x7f000001))
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (port == 4242)
        *output = &socketHandle;
    else if (port == 4243)
        *output = &closeFailureHandle;
    else if (port == 4244)
        *output = &automaticHandle;
    else
        return FLECHA_RUNTIME_NETWORK_FAILED;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_udp_socket_send_to(
    void* handle, const uint8_t* source, std::size_t length, uint8_t family,
    uint64_t high, uint64_t low, uint16_t port, std::size_t* count) {
    if (count != nullptr) *count = 0;
    if (handle != &socketHandle || source == nullptr || length != 3 ||
        family != 4 || high != 0 || low != UINT64_C(0x7f000001) ||
        port != 9000 || count == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (sendCalls++ == 0) {
        *count = 2;
        return FLECHA_RUNTIME_NETWORK_UNREACHABLE;
    }
    *count = 1;
    return FLECHA_RUNTIME_NETWORK_FAILED;
}

extern "C" int32_t __wrap_flecha_runtime_v1_udp_socket_receive_from(
    void* handle, uint8_t* destination, std::size_t length, std::size_t* count,
    uint8_t* family, uint64_t* high, uint64_t* low, uint16_t* port) {
    if (count != nullptr) *count = 0;
    ResetAddress(family, high, low, port);
    if (handle != &socketHandle || destination == nullptr || length != 2 ||
        count == nullptr || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (receiveCalls++ == 0) {
        destination[0] = 42;
        *count = 1;
        *family = 6;
        *low = 1;
        *port = 65000;
        return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;
    }
    return FLECHA_RUNTIME_NETWORK_FAILED;
}

extern "C" int32_t __wrap_flecha_runtime_v1_udp_socket_set_timeout(
    void* handle, uint8_t direction, uint8_t enabled, uint64_t seconds,
    uint32_t nanoseconds) {
    if (handle != &socketHandle) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (direction == 0 && enabled == 1 && seconds == 1 &&
        nanoseconds == 250000000)
        timeoutMask |= 1;
    else if (direction == 0 && enabled == 0 && seconds == 0 && nanoseconds == 0)
        timeoutMask |= 2;
    else if (direction == 1 && enabled == 1 && seconds == 2 &&
             nanoseconds == 750000000)
        timeoutMask |= 4;
    else if (direction == 1 && enabled == 0 && seconds == 0 && nanoseconds == 0)
        timeoutMask |= 8;
    else
        return FLECHA_RUNTIME_NETWORK_FAILED;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_udp_socket_local_address(
    void* handle, uint8_t* family, uint64_t* high, uint64_t* low,
    uint16_t* port) {
    ResetAddress(family, high, low, port);
    if (handle != &socketHandle || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    *family = 4;
    *low = UINT64_C(0x7f000001);
    *port = 50000;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_udp_socket_close(void* handle) {
    if (handle != &socketHandle && handle != &closeFailureHandle &&
        handle != &automaticHandle)
        return FLECHA_RUNTIME_NETWORK_CLOSED;
    ++closeCount;
    return handle == &closeFailureHandle ? FLECHA_RUNTIME_NETWORK_FAILED
                                         : FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" std::size_t flecha_test_udp_close_count() { return closeCount; }
extern "C" uint8_t flecha_test_udp_timeout_mask() { return timeoutMask; }
