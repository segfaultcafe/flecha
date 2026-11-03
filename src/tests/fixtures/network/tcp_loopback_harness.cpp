#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "runtime/runtime_abi.h"

namespace {

constexpr uint64_t Loopback = UINT64_C(0x7f000001);
constexpr std::size_t PayloadSize = 8192;

std::thread serverThread;
std::thread clientThread;
std::atomic<int32_t> serverStatus{0};
std::atomic<int32_t> clientStatus{0};

bool SetTimeouts(void* stream) {
    return flecha_runtime_v1_tcp_stream_set_timeout(stream, 0, 1, 3, 0) ==
               FLECHA_RUNTIME_NETWORK_OK &&
           flecha_runtime_v1_tcp_stream_set_timeout(stream, 1, 1, 3, 0) ==
               FLECHA_RUNTIME_NETWORK_OK;
}

bool ReadAll(void* stream, uint8_t* destination, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        std::size_t count = 0;
        const int32_t status = flecha_runtime_v1_tcp_stream_read(
            stream, destination + offset, length - offset, &count);
        if (status != FLECHA_RUNTIME_NETWORK_OK || count == 0) return false;
        offset += count;
    }
    return true;
}

bool WriteAll(void* stream, const uint8_t* source, std::size_t length,
              std::size_t chunkSize) {
    std::size_t offset = 0;
    while (offset < length) {
        const std::size_t requested =
            std::min(chunkSize, static_cast<std::size_t>(length - offset));
        std::size_t count = 0;
        const int32_t status = flecha_runtime_v1_tcp_stream_write(
            stream, source + offset, requested, &count);
        if (status != FLECHA_RUNTIME_NETWORK_OK || count == 0) return false;
        offset += count;
    }
    return true;
}

bool HasRequestPattern(const std::vector<uint8_t>& bytes) {
    for (std::size_t index = 0; index < bytes.size(); ++index)
        if (bytes[index] != static_cast<uint8_t>(index % 251)) return false;
    return true;
}

bool HasResponsePattern(const std::vector<uint8_t>& bytes) {
    for (std::size_t index = 0; index < bytes.size(); ++index)
        if (bytes[index] !=
            static_cast<uint8_t>(static_cast<uint8_t>(index % 251) ^ 90))
            return false;
    return true;
}

void CloseStream(void* stream) {
    if (stream != nullptr) (void)flecha_runtime_v1_tcp_stream_close(stream);
}

}  // namespace

extern "C" uint16_t flecha_test_tcp_start_server() {
    if (serverThread.joinable()) {
        std::fputs("TCP test server thread is already active\n", stderr);
        return 0;
    }

    void* listener = nullptr;
    const int32_t bindStatus =
        flecha_runtime_v1_tcp_listener_bind(&listener, 4, 0, Loopback, 0, 16);
    if (bindStatus != FLECHA_RUNTIME_NETWORK_OK) {
        std::fprintf(stderr, "TCP test server bind failed: %d\n", bindStatus);
        return 0;
    }

    uint8_t family = 0;
    uint64_t high = 0;
    uint64_t low = 0;
    uint16_t port = 0;
    const int32_t addressStatus = flecha_runtime_v1_tcp_listener_local_address(
        listener, &family, &high, &low, &port);
    if (addressStatus != FLECHA_RUNTIME_NETWORK_OK || family != 4 ||
        high != 0 || low != Loopback || port == 0) {
        std::fprintf(stderr,
                     "TCP test server address failed: status=%d family=%u "
                     "high=%llu low=%llu port=%u\n",
                     addressStatus, static_cast<unsigned>(family),
                     static_cast<unsigned long long>(high),
                     static_cast<unsigned long long>(low),
                     static_cast<unsigned>(port));
        (void)flecha_runtime_v1_tcp_listener_close(listener);
        return 0;
    }

    serverStatus = 100;
    serverThread = std::thread([listener] {
        void* stream = nullptr;
        uint8_t family = 0;
        uint64_t high = 0;
        uint64_t low = 0;
        uint16_t port = 0;
        if (flecha_runtime_v1_tcp_listener_accept(listener, &stream, &family,
                                                  &high, &low, &port) !=
                FLECHA_RUNTIME_NETWORK_OK ||
            stream == nullptr) {
            serverStatus = 101;
            (void)flecha_runtime_v1_tcp_listener_close(listener);
            return;
        }
        (void)flecha_runtime_v1_tcp_listener_close(listener);
        if (!SetTimeouts(stream)) {
            serverStatus = 102;
            CloseStream(stream);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::vector<uint8_t> payload(PayloadSize);
        if (!ReadAll(stream, payload.data(), payload.size()) ||
            !HasRequestPattern(payload)) {
            serverStatus = 103;
            CloseStream(stream);
            return;
        }
        for (uint8_t& byte : payload) byte ^= 90;
        if (!WriteAll(stream, payload.data(), payload.size(), 97)) {
            serverStatus = 104;
            CloseStream(stream);
            return;
        }
        if (flecha_runtime_v1_tcp_stream_shutdown(stream, 1) !=
            FLECHA_RUNTIME_NETWORK_OK) {
            serverStatus = 105;
            CloseStream(stream);
            return;
        }
        serverStatus = flecha_runtime_v1_tcp_stream_close(stream) ==
                               FLECHA_RUNTIME_NETWORK_OK
                           ? 0
                           : 106;
    });
    return port;
}

extern "C" int32_t flecha_test_tcp_join_server() {
    if (!serverThread.joinable()) return 107;
    serverThread.join();
    return serverStatus.load();
}

extern "C" int32_t flecha_test_tcp_start_client(uint16_t port) {
    if (port == 0 || clientThread.joinable()) return 201;
    clientStatus = 200;
    clientThread = std::thread([port] {
        void* stream = nullptr;
        if (flecha_runtime_v1_tcp_stream_connect(&stream, 4, 0, Loopback, port,
                                                 1, 3, 0) !=
                FLECHA_RUNTIME_NETWORK_OK ||
            stream == nullptr) {
            clientStatus = 202;
            return;
        }
        if (!SetTimeouts(stream)) {
            clientStatus = 203;
            CloseStream(stream);
            return;
        }

        std::vector<uint8_t> payload(PayloadSize);
        for (std::size_t index = 0; index < payload.size(); ++index)
            payload[index] = static_cast<uint8_t>(index % 251);
        if (!WriteAll(stream, payload.data(), payload.size(), 61)) {
            clientStatus = 204;
            CloseStream(stream);
            return;
        }
        if (flecha_runtime_v1_tcp_stream_shutdown(stream, 1) !=
            FLECHA_RUNTIME_NETWORK_OK) {
            clientStatus = 205;
            CloseStream(stream);
            return;
        }

        std::vector<uint8_t> response(PayloadSize);
        if (!ReadAll(stream, response.data(), response.size()) ||
            !HasResponsePattern(response)) {
            clientStatus = 206;
            CloseStream(stream);
            return;
        }
        clientStatus = flecha_runtime_v1_tcp_stream_close(stream) ==
                               FLECHA_RUNTIME_NETWORK_OK
                           ? 0
                           : 207;
    });
    return 0;
}

extern "C" int32_t flecha_test_tcp_join_client() {
    if (!clientThread.joinable()) return 208;
    clientThread.join();
    return clientStatus.load();
}
