#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace {

struct NetworkAddress final {
    uint8_t family;
    uint64_t high;
    uint64_t low;

    bool operator==(const NetworkAddress&) const = default;
};

struct DnsResults final {
    std::vector<NetworkAddress> addresses;
};

bool ValidHostname(const char* name, size_t length) {
    if (name == nullptr || length == 0 || length > 253) return false;

    size_t labelLength = 0;
    bool labelStartsWithHyphen = false;
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(name[index]);
        if (byte == '.') {
            if (labelLength == 0 || labelStartsWithHyphen ||
                name[index - 1] == '-')
                return false;
            labelLength = 0;
            labelStartsWithHyphen = false;
            continue;
        }

        const bool letter =
            (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit && byte != '-') return false;
        if (labelLength == 0) labelStartsWithHyphen = byte == '-';
        ++labelLength;
        if (labelLength > 63) return false;
    }

    return labelLength == 0 ||
           (!labelStartsWithHyphen && name[length - 1] != '-');
}

int32_t MapResolverError(int error) {
    if (error == EAI_AGAIN) return FLECHA_RUNTIME_NETWORK_TEMPORARY_FAILURE;
    if (error == EAI_NONAME) return FLECHA_RUNTIME_NETWORK_NAME_NOT_FOUND;
#ifdef EAI_NODATA
    if (error == EAI_NODATA) return FLECHA_RUNTIME_NETWORK_NAME_NOT_FOUND;
#endif
    if (error == EAI_FAMILY) return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
#ifdef EAI_ADDRFAMILY
    if (error == EAI_ADDRFAMILY)
        return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
#endif
    if (error == EAI_MEMORY) return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
#ifndef _WIN32
    if (error == EAI_SYSTEM && errno == ENOMEM)
        return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
#endif
    return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
}

uint64_t ReadBigEndian(const uint8_t* bytes, size_t length) {
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index)
        value = (value << 8) | bytes[index];
    return value;
}

void AppendUnique(DnsResults& output, NetworkAddress address) {
    for (const NetworkAddress& existing : output.addresses)
        if (existing == address) return;
    output.addresses.push_back(address);
}

}  // namespace

extern "C" {

int32_t flecha_runtime_v1_dns_resolve(void** output, const char* name,
                                      size_t nameLength) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
    *output = nullptr;
    if (!ValidHostname(name, nameLength))
        return FLECHA_RUNTIME_NETWORK_INVALID_NAME;

#ifdef _WIN32
    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0)
        return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
#endif

    int32_t status = FLECHA_RUNTIME_NETWORK_OK;
    addrinfo* resolved = nullptr;
    try {
        std::string nativeName(name, nameLength);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        const int resolutionStatus =
            getaddrinfo(nativeName.c_str(), nullptr, &hints, &resolved);
        if (resolutionStatus != 0) {
            status = MapResolverError(resolutionStatus);
        } else {
            auto results = new DnsResults{};
            try {
                for (const addrinfo* current = resolved; current != nullptr;
                     current = current->ai_next) {
                    if (current->ai_family == AF_INET &&
                        current->ai_addrlen >= sizeof(sockaddr_in)) {
                        const auto* address =
                            reinterpret_cast<const sockaddr_in*>(
                                current->ai_addr);
                        const auto* bytes = reinterpret_cast<const uint8_t*>(
                            &address->sin_addr);
                        AppendUnique(*results, {4, 0, ReadBigEndian(bytes, 4)});
                    } else if (current->ai_family == AF_INET6 &&
                               current->ai_addrlen >= sizeof(sockaddr_in6)) {
                        const auto* address =
                            reinterpret_cast<const sockaddr_in6*>(
                                current->ai_addr);
                        if (address->sin6_scope_id != 0) continue;
                        const auto* bytes = reinterpret_cast<const uint8_t*>(
                            &address->sin6_addr);
                        AppendUnique(*results, {6, ReadBigEndian(bytes, 8),
                                                ReadBigEndian(bytes + 8, 8)});
                    }
                }
            } catch (...) {
                delete results;
                throw;
            }

            if (results->addresses.empty()) {
                delete results;
                status = FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
            } else {
                *output = results;
            }
        }
    } catch (const std::bad_alloc&) {
        status = FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
    } catch (...) {
        status = FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;
    }

    if (resolved != nullptr) freeaddrinfo(resolved);
#ifdef _WIN32
    WSACleanup();
#endif
    return status;
}

size_t flecha_runtime_v1_dns_result_count(const void* handle) {
    if (handle == nullptr) return 0;
    return static_cast<const DnsResults*>(handle)->addresses.size();
}

int32_t flecha_runtime_v1_dns_result_get(const void* handle, size_t index,
                                         uint8_t* family, uint64_t* high,
                                         uint64_t* low) {
    if (family != nullptr) *family = 0;
    if (high != nullptr) *high = 0;
    if (low != nullptr) *low = 0;
    if (handle == nullptr || family == nullptr || high == nullptr ||
        low == nullptr)
        return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;

    const auto& addresses = static_cast<const DnsResults*>(handle)->addresses;
    if (index >= addresses.size())
        return FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED;

    const NetworkAddress& address = addresses[index];
    *family = address.family;
    *high = address.high;
    *low = address.low;
    return FLECHA_RUNTIME_NETWORK_OK;
}

void flecha_runtime_v1_dns_result_close(void* handle) {
    delete static_cast<DnsResults*>(handle);
}

}  // extern "C"
