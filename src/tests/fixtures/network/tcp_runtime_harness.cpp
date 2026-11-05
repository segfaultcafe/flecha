#include <cstddef>
#include <cstdint>

#include "runtime/runtime_abi.h"

namespace {

int streamHandle;
int timedStreamHandle;
int closeFailureHandle;
int acceptedStreamHandle;
int listenerHandle;

std::size_t streamCloseCount;
std::size_t listenerCloseCount;
uint8_t timeoutMask;
uint8_t connectTimeoutValid;
uint8_t bindValid;
uint8_t shutdownValid;
unsigned readCalls;
unsigned writeCalls;

void ResetAddress(uint8_t* family, uint64_t* high, uint64_t* low,
                  uint16_t* port) {
    if (family != nullptr) *family = 0;
    if (high != nullptr) *high = 0;
    if (low != nullptr) *low = 0;
    if (port != nullptr) *port = 0;
}

}  // namespace

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_connect(
    void** output, uint8_t family, uint64_t high, uint64_t low, uint16_t port,
    uint8_t timeoutEnabled, uint64_t timeoutSeconds,
    uint32_t timeoutNanoseconds) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = nullptr;

    switch (port) {
        case 10001:
            return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
        case 10007:
            return FLECHA_RUNTIME_NETWORK_CLOSED;
        case 10008:
            return FLECHA_RUNTIME_NETWORK_TIMED_OUT;
        case 10009:
            return FLECHA_RUNTIME_NETWORK_CONNECTION_REFUSED;
        case 10010:
            return FLECHA_RUNTIME_NETWORK_CONNECTION_RESET;
        case 10011:
            return FLECHA_RUNTIME_NETWORK_UNREACHABLE;
        case 10012:
            return FLECHA_RUNTIME_NETWORK_ADDRESS_IN_USE;
        case 10013:
            return FLECHA_RUNTIME_NETWORK_ADDRESS_NOT_AVAILABLE;
        case 10014:
            return FLECHA_RUNTIME_NETWORK_BROKEN_PIPE;
        case 10015:
            return FLECHA_RUNTIME_NETWORK_FAILED;
        default:
            break;
    }

    if (family != 4 || high != 0 || low != 0x7f000001)
        return FLECHA_RUNTIME_NETWORK_FAILED;

    if (port == 4242 && timeoutEnabled == 0) {
        *output = &streamHandle;
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    if (port == 4243 && timeoutEnabled == 1 && timeoutSeconds == 1 &&
        timeoutNanoseconds == 500000000) {
        connectTimeoutValid = 1;
        *output = &timedStreamHandle;
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    if (port == 4244 && timeoutEnabled == 0) {
        *output = &closeFailureHandle;
        return FLECHA_RUNTIME_NETWORK_OK;
    }

    return FLECHA_RUNTIME_NETWORK_FAILED;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_listener_bind(
    void** output, uint8_t family, uint64_t high, uint64_t low, uint16_t port,
    uint32_t backlog) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = nullptr;
    if (family != 4 || high != 0 || low != 0x7f000001 || port != 0 ||
        backlog != 16)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    bindValid = 1;
    *output = &listenerHandle;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_listener_accept(
    void* listener, void** output, uint8_t* family, uint64_t* high,
    uint64_t* low, uint16_t* port) {
    if (output != nullptr) *output = nullptr;
    ResetAddress(family, high, low, port);
    if (listener != &listenerHandle || output == nullptr || family == nullptr ||
        high == nullptr || low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = &acceptedStreamHandle;
    *family = 6;
    *low = 1;
    *port = 65000;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_read(
    void* handle, uint8_t* destination, std::size_t length,
    std::size_t* count) {
    if (count != nullptr) *count = 0;
    if (handle != &streamHandle || count == nullptr ||
        (destination == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (readCalls++ == 0 && length != 0) {
        destination[0] = 42;
        *count = 1;
        return FLECHA_RUNTIME_NETWORK_CONNECTION_RESET;
    }
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_write(
    void* handle, const uint8_t* source, std::size_t length,
    std::size_t* count) {
    if (count != nullptr) *count = 0;
    if (handle != &streamHandle || count == nullptr ||
        (source == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (writeCalls++ == 0) {
        *count = length < 2 ? length : 2;
        return FLECHA_RUNTIME_NETWORK_BROKEN_PIPE;
    }
    *count = length;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_set_timeout(
    void* handle, uint8_t direction, uint8_t enabled, uint64_t seconds,
    uint32_t nanoseconds) {
    if (handle != &streamHandle) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (direction == 0 && enabled == 1 && seconds == 2 &&
        nanoseconds == 500000000)
        timeoutMask |= 1;
    else if (direction == 0 && enabled == 0 && seconds == 0 &&
             nanoseconds == 0)
        timeoutMask |= 2;
    else if (direction == 1 && enabled == 1 && seconds == 3 &&
             nanoseconds == 750000000)
        timeoutMask |= 4;
    else if (direction == 1 && enabled == 0 && seconds == 0 &&
             nanoseconds == 0)
        timeoutMask |= 8;
    else
        return FLECHA_RUNTIME_NETWORK_FAILED;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_shutdown(
    void* handle, uint8_t direction) {
    if (handle != &streamHandle || direction != 2)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    shutdownValid = 1;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_local_address(
    void* handle, uint8_t* family, uint64_t* high, uint64_t* low,
    uint16_t* port) {
    ResetAddress(family, high, low, port);
    if (handle != &streamHandle || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    *family = 4;
    *low = 0x7f000001;
    *port = 50000;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_peer_address(
    void* handle, uint8_t* family, uint64_t* high, uint64_t* low,
    uint16_t* port) {
    ResetAddress(family, high, low, port);
    if (handle != &streamHandle || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    *family = 4;
    *low = 0x7f000001;
    *port = 443;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_listener_local_address(
    void* handle, uint8_t* family, uint64_t* high, uint64_t* low,
    uint16_t* port) {
    ResetAddress(family, high, low, port);
    if (handle != &listenerHandle || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    *family = 4;
    *low = 0x7f000001;
    *port = 4321;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_stream_close(void* handle) {
    if (handle != &streamHandle && handle != &timedStreamHandle &&
        handle != &closeFailureHandle && handle != &acceptedStreamHandle)
        return FLECHA_RUNTIME_NETWORK_CLOSED;
    ++streamCloseCount;
    if (handle == &closeFailureHandle) return FLECHA_RUNTIME_NETWORK_FAILED;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" int32_t __wrap_flecha_runtime_v1_tcp_listener_close(void* handle) {
    if (handle != &listenerHandle) return FLECHA_RUNTIME_NETWORK_CLOSED;
    ++listenerCloseCount;
    return FLECHA_RUNTIME_NETWORK_OK;
}

extern "C" std::size_t flecha_test_tcp_stream_close_count() {
    return streamCloseCount;
}

extern "C" std::size_t flecha_test_tcp_listener_close_count() {
    return listenerCloseCount;
}

extern "C" uint8_t flecha_test_tcp_timeout_mask() { return timeoutMask; }
extern "C" uint8_t flecha_test_tcp_connect_timeout_valid() {
    return connectTimeoutValid;
}
extern "C" uint8_t flecha_test_tcp_bind_valid() { return bindValid; }
extern "C" uint8_t flecha_test_tcp_shutdown_valid() {
    return shutdownValid;
}
