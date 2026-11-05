#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
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

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeSocketLength = int;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
using NativeSocketLength = socklen_t;
constexpr NativeSocket InvalidSocket = -1;
#endif

enum class TcpHandleKind : uint8_t { Stream, Listener };

struct TcpTimeout final {
    bool enabled = false;
    uint64_t seconds = 0;
    uint32_t nanoseconds = 0;
};

struct TcpHandle final {
    static constexpr uint64_t Magic = UINT64_C(0x464c454348415443);

    uint64_t magic = Magic;
    NativeSocket socket = InvalidSocket;
    TcpHandleKind kind = TcpHandleKind::Stream;
    TcpTimeout readTimeout{};
    TcpTimeout writeTimeout{};
};

struct UdpHandle final {
    static constexpr uint64_t Magic = UINT64_C(0x464c454348415544);

    uint64_t magic = Magic;
    NativeSocket socket = InvalidSocket;
    TcpTimeout receiveTimeout{};
    TcpTimeout sendTimeout{};
};

constexpr size_t MaxUdpPayloadSize = 65507;
constexpr int UdpBufferCapacity = 131072;

using SteadyClock = std::chrono::steady_clock;

struct Deadline final {
    bool enabled;
    SteadyClock::time_point end;
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

int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool SocketErrorInterrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

bool SocketErrorWouldBlock(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

bool SocketErrorInProgress(int error) {
#ifdef _WIN32
    return error == WSAEINPROGRESS || error == WSAEALREADY ||
           error == WSAEWOULDBLOCK;
#else
    return error == EINPROGRESS || error == EALREADY || error == EWOULDBLOCK;
#endif
}

int32_t MapSocketError(int error) {
#ifdef _WIN32
    switch (error) {
        case WSAENOTSOCK:
        case WSAEBADF:
            return FLECHA_RUNTIME_NETWORK_CLOSED;
        case WSAETIMEDOUT:
        case WSAEWOULDBLOCK:
            return FLECHA_RUNTIME_NETWORK_TIMED_OUT;
        case WSAECONNREFUSED:
            return FLECHA_RUNTIME_NETWORK_CONNECTION_REFUSED;
        case WSAECONNRESET:
        case WSAECONNABORTED:
            return FLECHA_RUNTIME_NETWORK_CONNECTION_RESET;
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAENETDOWN:
            return FLECHA_RUNTIME_NETWORK_UNREACHABLE;
        case WSAEADDRINUSE:
            return FLECHA_RUNTIME_NETWORK_ADDRESS_IN_USE;
        case WSAEADDRNOTAVAIL:
            return FLECHA_RUNTIME_NETWORK_ADDRESS_NOT_AVAILABLE;
        case WSAESHUTDOWN:
            return FLECHA_RUNTIME_NETWORK_BROKEN_PIPE;
        case WSAENOBUFS:
            return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
        case WSAEAFNOSUPPORT:
            return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
        case WSAEMSGSIZE:
            return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;
        default:
            return FLECHA_RUNTIME_NETWORK_FAILED;
    }
#else
    switch (error) {
        case EBADF:
        case ENOTSOCK:
            return FLECHA_RUNTIME_NETWORK_CLOSED;
        case ETIMEDOUT:
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return FLECHA_RUNTIME_NETWORK_TIMED_OUT;
        case ECONNREFUSED:
            return FLECHA_RUNTIME_NETWORK_CONNECTION_REFUSED;
        case ECONNRESET:
        case ECONNABORTED:
            return FLECHA_RUNTIME_NETWORK_CONNECTION_RESET;
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ENETDOWN:
            return FLECHA_RUNTIME_NETWORK_UNREACHABLE;
        case EADDRINUSE:
            return FLECHA_RUNTIME_NETWORK_ADDRESS_IN_USE;
        case EADDRNOTAVAIL:
            return FLECHA_RUNTIME_NETWORK_ADDRESS_NOT_AVAILABLE;
        case EPIPE:
#ifdef ESHUTDOWN
        case ESHUTDOWN:
#endif
            return FLECHA_RUNTIME_NETWORK_BROKEN_PIPE;
        case ENOMEM:
        case ENOBUFS:
            return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
        case EAFNOSUPPORT:
            return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
        case EMSGSIZE:
            return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;
        default:
            return FLECHA_RUNTIME_NETWORK_FAILED;
    }
#endif
}

bool AcquireSocketRuntime() {
#ifdef _WIN32
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void ReleaseSocketRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}

int CloseNativeSocket(NativeSocket socket) {
#ifdef _WIN32
    return closesocket(socket);
#else
    return close(socket);
#endif
}

int32_t ConfigureNonblocking(NativeSocket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) != 0)
        return MapSocketError(LastSocketError());
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0)
        return MapSocketError(errno);
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0)
        return MapSocketError(errno);
#endif
#endif
    return FLECHA_RUNTIME_NETWORK_OK;
}

int32_t ConfigureIpv6Only(NativeSocket socket) {
    const int enabled = 1;
#ifdef _WIN32
    if (setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&enabled),
                   static_cast<int>(sizeof(enabled))) != 0)
#else
    if (setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, &enabled,
                   sizeof(enabled)) != 0)
#endif
        return MapSocketError(LastSocketError());
    return FLECHA_RUNTIME_NETWORK_OK;
}

int32_t ConfigureUdpBuffers(NativeSocket socket) {
#ifdef _WIN32
    const auto* capacity = reinterpret_cast<const char*>(&UdpBufferCapacity);
    const int capacityLength = static_cast<int>(sizeof(UdpBufferCapacity));
#else
    const auto* capacity = &UdpBufferCapacity;
    const socklen_t capacityLength = sizeof(UdpBufferCapacity);
#endif
    if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF, capacity, capacityLength) !=
            0 ||
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF, capacity, capacityLength) !=
            0)
        return MapSocketError(LastSocketError());
    return FLECHA_RUNTIME_NETWORK_OK;
}

void WriteBigEndian(uint8_t* output, uint64_t value, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        output[length - index - 1] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

int32_t MakeNativeAddress(uint8_t family, uint64_t high, uint64_t low,
                          uint16_t port, sockaddr_storage* output,
                          NativeSocketLength* outputLength) {
    if (output == nullptr || outputLength == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = {};
    *outputLength = 0;

    if (family == 4) {
        if (high != 0 || low > UINT32_MAX)
            return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
        auto* address = reinterpret_cast<sockaddr_in*>(output);
        address->sin_family = AF_INET;
        address->sin_port = htons(port);
        address->sin_addr.s_addr = htonl(static_cast<uint32_t>(low));
        *outputLength = static_cast<NativeSocketLength>(sizeof(sockaddr_in));
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    if (family == 6) {
        auto* address = reinterpret_cast<sockaddr_in6*>(output);
        address->sin6_family = AF_INET6;
        address->sin6_port = htons(port);
        auto* bytes = reinterpret_cast<uint8_t*>(&address->sin6_addr);
        WriteBigEndian(bytes, high, 8);
        WriteBigEndian(bytes + 8, low, 8);
        *outputLength = static_cast<NativeSocketLength>(sizeof(sockaddr_in6));
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
}

void ResetEndpoint(uint8_t* family, uint64_t* high, uint64_t* low,
                   uint16_t* port) {
    if (family != nullptr) *family = 0;
    if (high != nullptr) *high = 0;
    if (low != nullptr) *low = 0;
    if (port != nullptr) *port = 0;
}

int32_t ReadNativeAddress(const sockaddr* address, NativeSocketLength length,
                          uint8_t* family, uint64_t* high, uint64_t* low,
                          uint16_t* port) {
    ResetEndpoint(family, high, low, port);
    if (address == nullptr || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;

    if (address->sa_family == AF_INET &&
        length >= static_cast<NativeSocketLength>(sizeof(sockaddr_in))) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&ipv4->sin_addr);
        *family = 4;
        *low = ReadBigEndian(bytes, 4);
        *port = ntohs(ipv4->sin_port);
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    if (address->sa_family == AF_INET6 &&
        length >= static_cast<NativeSocketLength>(sizeof(sockaddr_in6))) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (ipv6->sin6_scope_id != 0)
            return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&ipv6->sin6_addr);
        *family = 6;
        *high = ReadBigEndian(bytes, 8);
        *low = ReadBigEndian(bytes + 8, 8);
        *port = ntohs(ipv6->sin6_port);
        return FLECHA_RUNTIME_NETWORK_OK;
    }
    return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;
}

TcpHandle* GetHandle(void* opaque, TcpHandleKind kind) {
    if (opaque == nullptr) return nullptr;
    auto* handle = static_cast<TcpHandle*>(opaque);
    if (handle->magic != TcpHandle::Magic || handle->kind != kind ||
        handle->socket == InvalidSocket)
        return nullptr;
    return handle;
}

Deadline MakeDeadline(const TcpTimeout& timeout) {
    if (!timeout.enabled) return {false, {}};
    const SteadyClock::time_point now = SteadyClock::now();
    const auto available = SteadyClock::time_point::max() - now;
    const auto availableSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(available).count();
    if (timeout.seconds > static_cast<uint64_t>(availableSeconds))
        return {true, SteadyClock::time_point::max()};
    auto end = now + std::chrono::seconds(timeout.seconds);
    const auto nanos = std::chrono::nanoseconds(timeout.nanoseconds);
    if (SteadyClock::time_point::max() - end < nanos)
        end = SteadyClock::time_point::max();
    else
        end += nanos;
    return {true, end};
}

int32_t WaitForSocket(NativeSocket socket, bool writable,
                      const Deadline& deadline) {
    for (;;) {
#ifdef _WIN32
        fd_set readSet;
        fd_set writeSet;
        fd_set errorSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        if (writable)
            FD_SET(socket, &writeSet);
        else
            FD_SET(socket, &readSet);
        FD_SET(socket, &errorSet);

        timeval value{};
        timeval* timeout = nullptr;
        if (deadline.enabled) {
            const auto now = SteadyClock::now();
            const auto remaining = deadline.end > now
                                       ? deadline.end - now
                                       : SteadyClock::duration::zero();
            const auto wholeMicros =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    remaining);
            auto micros = wholeMicros.count();
            if (wholeMicros < remaining && micros < INT64_MAX) ++micros;
            micros = std::min<int64_t>(micros, LONG_MAX);
            value.tv_sec = static_cast<long>(micros / 1000000);
            value.tv_usec = static_cast<long>(micros % 1000000);
            timeout = &value;
        }
        const int result = select(0, &readSet, &writeSet, &errorSet, timeout);
#else
        pollfd descriptor{};
        descriptor.fd = socket;
        descriptor.events = writable ? POLLOUT : POLLIN;
        int timeout = -1;
        if (deadline.enabled) {
            const auto now = SteadyClock::now();
            const auto remaining = deadline.end > now
                                       ? deadline.end - now
                                       : SteadyClock::duration::zero();
            const auto wholeMillis =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    remaining);
            auto millis = wholeMillis.count();
            if (wholeMillis < remaining && millis < INT64_MAX) ++millis;
            timeout = static_cast<int>(std::min<int64_t>(millis, INT_MAX));
        }
        const int result = poll(&descriptor, 1, timeout);
#endif
        if (result > 0) return FLECHA_RUNTIME_NETWORK_OK;
        if (result == 0) {
            if (deadline.enabled && SteadyClock::now() < deadline.end) continue;
            return FLECHA_RUNTIME_NETWORK_TIMED_OUT;
        }
        const int error = LastSocketError();
        if (SocketErrorInterrupted(error)) continue;
        return MapSocketError(error);
    }
}

int32_t CreateSocket(uint8_t family, bool listener, NativeSocket* output) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = InvalidSocket;
    const int nativeFamily = family == 4 ? AF_INET : family == 6 ? AF_INET6 : 0;
    if (nativeFamily == 0) return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;

    const NativeSocket socket =
        ::socket(nativeFamily, SOCK_STREAM, IPPROTO_TCP);
    if (socket == InvalidSocket) return MapSocketError(LastSocketError());

    int32_t status = ConfigureNonblocking(socket);
    if (status == FLECHA_RUNTIME_NETWORK_OK && listener && family == 6)
        status = ConfigureIpv6Only(socket);
    if (status != FLECHA_RUNTIME_NETWORK_OK) {
        CloseNativeSocket(socket);
        return status;
    }
    *output = socket;
    return FLECHA_RUNTIME_NETWORK_OK;
}

int32_t WrapSocket(NativeSocket socket, TcpHandleKind kind, void** output) {
    try {
        *output = new TcpHandle{TcpHandle::Magic, socket, kind, {}, {}};
        return FLECHA_RUNTIME_NETWORK_OK;
    } catch (const std::bad_alloc&) {
        return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
    } catch (...) {
        return FLECHA_RUNTIME_NETWORK_FAILED;
    }
}

int32_t SocketAddress(void* opaque, TcpHandleKind kind, bool peer,
                      uint8_t* family, uint64_t* high, uint64_t* low,
                      uint16_t* port) {
    ResetEndpoint(family, high, low, port);
    TcpHandle* handle = GetHandle(opaque, kind);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (family == nullptr || high == nullptr || low == nullptr ||
        port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;

    sockaddr_storage address{};
    NativeSocketLength length =
        static_cast<NativeSocketLength>(sizeof(address));
    const int result =
        peer ? getpeername(handle->socket,
                           reinterpret_cast<sockaddr*>(&address), &length)
             : getsockname(handle->socket,
                           reinterpret_cast<sockaddr*>(&address), &length);
    if (result != 0) return MapSocketError(LastSocketError());
    return ReadNativeAddress(reinterpret_cast<const sockaddr*>(&address),
                             length, family, high, low, port);
}

int32_t CloseTcpHandle(void* opaque, TcpHandleKind kind) {
    TcpHandle* handle = GetHandle(opaque, kind);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    const NativeSocket socket = handle->socket;
    handle->socket = InvalidSocket;
    handle->magic = 0;
    const int result = CloseNativeSocket(socket);
    const int error = result == 0 ? 0 : LastSocketError();
    delete handle;
    ReleaseSocketRuntime();
    return result == 0 ? FLECHA_RUNTIME_NETWORK_OK : MapSocketError(error);
}

UdpHandle* GetUdpHandle(void* opaque) {
    if (opaque == nullptr) return nullptr;
    auto* handle = static_cast<UdpHandle*>(opaque);
    if (handle->magic != UdpHandle::Magic || handle->socket == InvalidSocket)
        return nullptr;
    return handle;
}

int32_t CreateUdpSocket(uint8_t family, NativeSocket* output) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = InvalidSocket;
    const int nativeFamily = family == 4 ? AF_INET : family == 6 ? AF_INET6 : 0;
    if (nativeFamily == 0) return FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY;

    const NativeSocket socket = ::socket(nativeFamily, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == InvalidSocket) return MapSocketError(LastSocketError());

    int32_t status = ConfigureNonblocking(socket);
    if (status == FLECHA_RUNTIME_NETWORK_OK)
        status = ConfigureUdpBuffers(socket);
    if (status == FLECHA_RUNTIME_NETWORK_OK && family == 6)
        status = ConfigureIpv6Only(socket);
    if (status != FLECHA_RUNTIME_NETWORK_OK) {
        CloseNativeSocket(socket);
        return status;
    }
    *output = socket;
    return FLECHA_RUNTIME_NETWORK_OK;
}

int32_t WrapUdpSocket(NativeSocket socket, void** output) {
    try {
        *output = new UdpHandle{UdpHandle::Magic, socket, {}, {}};
        return FLECHA_RUNTIME_NETWORK_OK;
    } catch (const std::bad_alloc&) {
        return FLECHA_RUNTIME_NETWORK_OUT_OF_MEMORY;
    } catch (...) {
        return FLECHA_RUNTIME_NETWORK_FAILED;
    }
}

int32_t UdpSocketAddress(void* opaque, bool peer, uint8_t* family,
                         uint64_t* high, uint64_t* low, uint16_t* port) {
    ResetEndpoint(family, high, low, port);
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (family == nullptr || high == nullptr || low == nullptr ||
        port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;

    sockaddr_storage address{};
    NativeSocketLength length =
        static_cast<NativeSocketLength>(sizeof(address));
    const int result =
        peer ? getpeername(handle->socket,
                           reinterpret_cast<sockaddr*>(&address), &length)
             : getsockname(handle->socket,
                           reinterpret_cast<sockaddr*>(&address), &length);
    if (result != 0) return MapSocketError(LastSocketError());
    return ReadNativeAddress(reinterpret_cast<const sockaddr*>(&address),
                             length, family, high, low, port);
}

int32_t CloseUdpHandle(void* opaque) {
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    const NativeSocket socket = handle->socket;
    handle->socket = InvalidSocket;
    handle->magic = 0;
    const int result = CloseNativeSocket(socket);
    const int error = result == 0 ? 0 : LastSocketError();
    delete handle;
    ReleaseSocketRuntime();
    return result == 0 ? FLECHA_RUNTIME_NETWORK_OK : MapSocketError(error);
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

int32_t flecha_runtime_v1_tcp_stream_connect(void** output, uint8_t family,
                                             uint64_t high, uint64_t low,
                                             uint16_t port,
                                             uint8_t timeoutEnabled,
                                             uint64_t timeoutSeconds,
                                             uint32_t timeoutNanoseconds) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = nullptr;
    if (timeoutEnabled > 1 || timeoutNanoseconds >= 1000000000)
        return FLECHA_RUNTIME_NETWORK_FAILED;

    sockaddr_storage address{};
    NativeSocketLength addressLength = 0;
    int32_t status =
        MakeNativeAddress(family, high, low, port, &address, &addressLength);
    if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
    if (!AcquireSocketRuntime()) return FLECHA_RUNTIME_NETWORK_FAILED;

    NativeSocket socket = InvalidSocket;
    status = CreateSocket(family, false, &socket);
    if (status != FLECHA_RUNTIME_NETWORK_OK) {
        ReleaseSocketRuntime();
        return status;
    }

    const int result =
        connect(socket, reinterpret_cast<sockaddr*>(&address), addressLength);
    if (result != 0) {
        const int error = LastSocketError();
        if (!SocketErrorInProgress(error)) {
            status = MapSocketError(error);
        } else {
            const TcpTimeout timeout{timeoutEnabled != 0, timeoutSeconds,
                                     timeoutNanoseconds};
            status = WaitForSocket(socket, true, MakeDeadline(timeout));
            if (status == FLECHA_RUNTIME_NETWORK_OK) {
                int socketError = 0;
                NativeSocketLength length =
                    static_cast<NativeSocketLength>(sizeof(socketError));
#ifdef _WIN32
                const int optionStatus =
                    getsockopt(socket, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socketError), &length);
#else
                const int optionStatus = getsockopt(
                    socket, SOL_SOCKET, SO_ERROR, &socketError, &length);
#endif
                if (optionStatus != 0)
                    status = MapSocketError(LastSocketError());
                else if (socketError != 0)
                    status = MapSocketError(socketError);
            }
        }
    }

    if (status == FLECHA_RUNTIME_NETWORK_OK)
        status = WrapSocket(socket, TcpHandleKind::Stream, output);
    if (status != FLECHA_RUNTIME_NETWORK_OK) {
        CloseNativeSocket(socket);
        ReleaseSocketRuntime();
    }
    return status;
}

int32_t flecha_runtime_v1_tcp_listener_bind(void** output, uint8_t family,
                                            uint64_t high, uint64_t low,
                                            uint16_t port, uint32_t backlog) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = nullptr;

    sockaddr_storage address{};
    NativeSocketLength addressLength = 0;
    int32_t status =
        MakeNativeAddress(family, high, low, port, &address, &addressLength);
    if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
    if (!AcquireSocketRuntime()) return FLECHA_RUNTIME_NETWORK_FAILED;

    NativeSocket socket = InvalidSocket;
    status = CreateSocket(family, true, &socket);
    if (status == FLECHA_RUNTIME_NETWORK_OK &&
        bind(socket, reinterpret_cast<sockaddr*>(&address), addressLength) != 0)
        status = MapSocketError(LastSocketError());
    if (status == FLECHA_RUNTIME_NETWORK_OK &&
        listen(socket,
               static_cast<int>(std::min<uint32_t>(backlog, INT_MAX))) != 0)
        status = MapSocketError(LastSocketError());
    if (status == FLECHA_RUNTIME_NETWORK_OK)
        status = WrapSocket(socket, TcpHandleKind::Listener, output);
    if (status != FLECHA_RUNTIME_NETWORK_OK) {
        if (socket != InvalidSocket) CloseNativeSocket(socket);
        ReleaseSocketRuntime();
    }
    return status;
}

int32_t flecha_runtime_v1_tcp_listener_accept(void* listener, void** output,
                                              uint8_t* family, uint64_t* high,
                                              uint64_t* low, uint16_t* port) {
    if (output != nullptr) *output = nullptr;
    ResetEndpoint(family, high, low, port);
    TcpHandle* listenerHandle = GetHandle(listener, TcpHandleKind::Listener);
    if (listenerHandle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (output == nullptr || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr)
        return FLECHA_RUNTIME_NETWORK_FAILED;

    for (;;) {
        const int32_t waitStatus =
            WaitForSocket(listenerHandle->socket, false, Deadline{false, {}});
        if (waitStatus != FLECHA_RUNTIME_NETWORK_OK) return waitStatus;

        sockaddr_storage address{};
        NativeSocketLength addressLength =
            static_cast<NativeSocketLength>(sizeof(address));
        const NativeSocket socket =
            accept(listenerHandle->socket,
                   reinterpret_cast<sockaddr*>(&address), &addressLength);
        if (socket == InvalidSocket) {
            const int error = LastSocketError();
            if (SocketErrorInterrupted(error) || SocketErrorWouldBlock(error))
                continue;
            return MapSocketError(error);
        }

        int32_t status = ConfigureNonblocking(socket);
        if (status == FLECHA_RUNTIME_NETWORK_OK)
            status =
                ReadNativeAddress(reinterpret_cast<const sockaddr*>(&address),
                                  addressLength, family, high, low, port);
        bool runtimeAcquired = false;
        if (status == FLECHA_RUNTIME_NETWORK_OK) {
            runtimeAcquired = AcquireSocketRuntime();
            if (!runtimeAcquired) status = FLECHA_RUNTIME_NETWORK_FAILED;
        }
        if (status == FLECHA_RUNTIME_NETWORK_OK)
            status = WrapSocket(socket, TcpHandleKind::Stream, output);
        if (status != FLECHA_RUNTIME_NETWORK_OK) {
            CloseNativeSocket(socket);
            if (runtimeAcquired) ReleaseSocketRuntime();
            ResetEndpoint(family, high, low, port);
        }
        return status;
    }
}

int32_t flecha_runtime_v1_tcp_stream_read(void* opaque, uint8_t* destination,
                                          size_t length, size_t* count) {
    if (count != nullptr) *count = 0;
    TcpHandle* handle = GetHandle(opaque, TcpHandleKind::Stream);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (count == nullptr || (destination == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (length == 0) return FLECHA_RUNTIME_NETWORK_OK;

    const Deadline deadline = MakeDeadline(handle->readTimeout);
    for (;;) {
        const int32_t status = WaitForSocket(handle->socket, false, deadline);
        if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
#ifdef _WIN32
        const int requested =
            static_cast<int>(std::min<size_t>(length, INT_MAX));
        const int result = recv(
            handle->socket, reinterpret_cast<char*>(destination), requested, 0);
#else
        const size_t requested = std::min<size_t>(
            length, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = recv(handle->socket, destination, requested, 0);
#endif
        if (result >= 0) {
            *count = static_cast<size_t>(result);
            return FLECHA_RUNTIME_NETWORK_OK;
        }
        const int error = LastSocketError();
        if (SocketErrorInterrupted(error) || SocketErrorWouldBlock(error))
            continue;
        return MapSocketError(error);
    }
}

int32_t flecha_runtime_v1_tcp_stream_write(void* opaque, const uint8_t* source,
                                           size_t length, size_t* count) {
    if (count != nullptr) *count = 0;
    TcpHandle* handle = GetHandle(opaque, TcpHandleKind::Stream);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (count == nullptr || (source == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (length == 0) return FLECHA_RUNTIME_NETWORK_OK;

    const Deadline deadline = MakeDeadline(handle->writeTimeout);
    for (;;) {
        const int32_t status = WaitForSocket(handle->socket, true, deadline);
        if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
#ifdef _WIN32
        const int requested =
            static_cast<int>(std::min<size_t>(length, INT_MAX));
        const int result =
            send(handle->socket, reinterpret_cast<const char*>(source),
                 requested, 0);
#else
        const size_t requested = std::min<size_t>(
            length, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
#ifdef MSG_NOSIGNAL
        constexpr int sendFlags = MSG_NOSIGNAL;
#else
        constexpr int sendFlags = 0;
#endif
        const ssize_t result =
            send(handle->socket, source, requested, sendFlags);
#endif
        if (result >= 0) {
            *count = static_cast<size_t>(result);
            return FLECHA_RUNTIME_NETWORK_OK;
        }
        const int error = LastSocketError();
        if (SocketErrorInterrupted(error) || SocketErrorWouldBlock(error))
            continue;
        return MapSocketError(error);
    }
}

int32_t flecha_runtime_v1_tcp_stream_set_timeout(void* opaque,
                                                 uint8_t direction,
                                                 uint8_t enabled,
                                                 uint64_t seconds,
                                                 uint32_t nanoseconds) {
    TcpHandle* handle = GetHandle(opaque, TcpHandleKind::Stream);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (direction > 1 || enabled > 1 || nanoseconds >= 1000000000)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    TcpTimeout& timeout =
        direction == 0 ? handle->readTimeout : handle->writeTimeout;
    timeout = TcpTimeout{enabled != 0, seconds, nanoseconds};
    return FLECHA_RUNTIME_NETWORK_OK;
}

int32_t flecha_runtime_v1_tcp_stream_shutdown(void* opaque, uint8_t direction) {
    TcpHandle* handle = GetHandle(opaque, TcpHandleKind::Stream);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (direction > 2) return FLECHA_RUNTIME_NETWORK_FAILED;
#ifdef _WIN32
    constexpr int directions[] = {SD_RECEIVE, SD_SEND, SD_BOTH};
#else
    constexpr int directions[] = {SHUT_RD, SHUT_WR, SHUT_RDWR};
#endif
    if (shutdown(handle->socket, directions[direction]) != 0)
        return MapSocketError(LastSocketError());
    return FLECHA_RUNTIME_NETWORK_OK;
}

int32_t flecha_runtime_v1_tcp_stream_local_address(void* handle,
                                                   uint8_t* family,
                                                   uint64_t* high,
                                                   uint64_t* low,
                                                   uint16_t* port) {
    return SocketAddress(handle, TcpHandleKind::Stream, false, family, high,
                         low, port);
}

int32_t flecha_runtime_v1_tcp_stream_peer_address(void* handle, uint8_t* family,
                                                  uint64_t* high, uint64_t* low,
                                                  uint16_t* port) {
    return SocketAddress(handle, TcpHandleKind::Stream, true, family, high, low,
                         port);
}

int32_t flecha_runtime_v1_tcp_listener_local_address(void* handle,
                                                     uint8_t* family,
                                                     uint64_t* high,
                                                     uint64_t* low,
                                                     uint16_t* port) {
    return SocketAddress(handle, TcpHandleKind::Listener, false, family, high,
                         low, port);
}

int32_t flecha_runtime_v1_tcp_stream_close(void* handle) {
    return CloseTcpHandle(handle, TcpHandleKind::Stream);
}

int32_t flecha_runtime_v1_tcp_listener_close(void* handle) {
    return CloseTcpHandle(handle, TcpHandleKind::Listener);
}

int32_t flecha_runtime_v1_udp_socket_bind(void** output, uint8_t family,
                                          uint64_t high, uint64_t low,
                                          uint16_t port) {
    if (output == nullptr) return FLECHA_RUNTIME_NETWORK_FAILED;
    *output = nullptr;

    sockaddr_storage address{};
    NativeSocketLength addressLength = 0;
    int32_t status =
        MakeNativeAddress(family, high, low, port, &address, &addressLength);
    if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
    if (!AcquireSocketRuntime()) return FLECHA_RUNTIME_NETWORK_FAILED;

    NativeSocket socket = InvalidSocket;
    status = CreateUdpSocket(family, &socket);
    if (status == FLECHA_RUNTIME_NETWORK_OK &&
        bind(socket, reinterpret_cast<sockaddr*>(&address), addressLength) != 0)
        status = MapSocketError(LastSocketError());
    if (status == FLECHA_RUNTIME_NETWORK_OK)
        status = WrapUdpSocket(socket, output);
    if (status != FLECHA_RUNTIME_NETWORK_OK) {
        if (socket != InvalidSocket) CloseNativeSocket(socket);
        ReleaseSocketRuntime();
    }
    return status;
}

int32_t flecha_runtime_v1_udp_socket_connect(void* opaque, uint8_t family,
                                             uint64_t high, uint64_t low,
                                             uint16_t port) {
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;

    sockaddr_storage address{};
    NativeSocketLength addressLength = 0;
    int32_t status =
        MakeNativeAddress(family, high, low, port, &address, &addressLength);
    if (status != FLECHA_RUNTIME_NETWORK_OK) return status;

    for (;;) {
        if (connect(handle->socket, reinterpret_cast<sockaddr*>(&address),
                    addressLength) == 0)
            return FLECHA_RUNTIME_NETWORK_OK;
        const int error = LastSocketError();
        if (SocketErrorInterrupted(error)) continue;
        return MapSocketError(error);
    }
}

int32_t flecha_runtime_v1_udp_socket_send_to(void* opaque,
                                             const uint8_t* source,
                                             size_t length, uint8_t family,
                                             uint64_t high, uint64_t low,
                                             uint16_t port, size_t* count) {
    if (count != nullptr) *count = 0;
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (count == nullptr || (source == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (length > MaxUdpPayloadSize)
        return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;

    sockaddr_storage address{};
    NativeSocketLength addressLength = 0;
    int32_t status =
        MakeNativeAddress(family, high, low, port, &address, &addressLength);
    if (status != FLECHA_RUNTIME_NETWORK_OK) return status;

    const Deadline deadline = MakeDeadline(handle->sendTimeout);
    for (;;) {
        status = WaitForSocket(handle->socket, true, deadline);
        if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
#ifdef _WIN32
        const int result =
            sendto(handle->socket, reinterpret_cast<const char*>(source),
                   static_cast<int>(length), 0,
                   reinterpret_cast<sockaddr*>(&address), addressLength);
#else
#ifdef MSG_NOSIGNAL
        constexpr int sendFlags = MSG_NOSIGNAL;
#else
        constexpr int sendFlags = 0;
#endif
        const ssize_t result =
            sendto(handle->socket, source, length, sendFlags,
                   reinterpret_cast<sockaddr*>(&address), addressLength);
#endif
        if (result >= 0) {
            *count = static_cast<size_t>(result);
            return FLECHA_RUNTIME_NETWORK_OK;
        }
        const int error = LastSocketError();
        if (SocketErrorInterrupted(error) || SocketErrorWouldBlock(error))
            continue;
        return MapSocketError(error);
    }
}

int32_t flecha_runtime_v1_udp_socket_send(void* opaque, const uint8_t* source,
                                          size_t length, size_t* count) {
    if (count != nullptr) *count = 0;
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (count == nullptr || (source == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
    if (length > MaxUdpPayloadSize)
        return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;

    const Deadline deadline = MakeDeadline(handle->sendTimeout);
    for (;;) {
        int32_t status = WaitForSocket(handle->socket, true, deadline);
        if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
#ifdef _WIN32
        const int result =
            send(handle->socket, reinterpret_cast<const char*>(source),
                 static_cast<int>(length), 0);
#else
#ifdef MSG_NOSIGNAL
        constexpr int sendFlags = MSG_NOSIGNAL;
#else
        constexpr int sendFlags = 0;
#endif
        const ssize_t result = send(handle->socket, source, length, sendFlags);
#endif
        if (result >= 0) {
            *count = static_cast<size_t>(result);
            return FLECHA_RUNTIME_NETWORK_OK;
        }
        const int error = LastSocketError();
        if (SocketErrorInterrupted(error) || SocketErrorWouldBlock(error))
            continue;
        return MapSocketError(error);
    }
}

int32_t flecha_runtime_v1_udp_socket_receive_from(
    void* opaque, uint8_t* destination, size_t length, size_t* count,
    uint8_t* family, uint64_t* high, uint64_t* low, uint16_t* port) {
    if (count != nullptr) *count = 0;
    ResetEndpoint(family, high, low, port);
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (count == nullptr || family == nullptr || high == nullptr ||
        low == nullptr || port == nullptr ||
        (destination == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
#ifdef _WIN32
    if (length > static_cast<size_t>(ULONG_MAX))
#else
    if (length > static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
#endif
        return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;

    const Deadline deadline = MakeDeadline(handle->receiveTimeout);
    for (;;) {
        int32_t status = WaitForSocket(handle->socket, false, deadline);
        if (status != FLECHA_RUNTIME_NETWORK_OK) return status;

        sockaddr_storage address{};
        NativeSocketLength addressLength =
            static_cast<NativeSocketLength>(sizeof(address));
#ifdef _WIN32
        WSABUF buffer{};
        buffer.buf = reinterpret_cast<char*>(destination);
        buffer.len = static_cast<ULONG>(length);
        DWORD received = 0;
        DWORD flags = 0;
        const int result =
            WSARecvFrom(handle->socket, &buffer, 1, &received, &flags,
                        reinterpret_cast<sockaddr*>(&address), &addressLength,
                        nullptr, nullptr);
        if (result == 0) {
            *count = static_cast<size_t>(received);
            return ReadNativeAddress(
                reinterpret_cast<const sockaddr*>(&address), addressLength,
                family, high, low, port);
        }
        const int error = LastSocketError();
        if (error == WSAEMSGSIZE) {
            *count = static_cast<size_t>(received);
            status =
                ReadNativeAddress(reinterpret_cast<const sockaddr*>(&address),
                                  addressLength, family, high, low, port);
            return status == FLECHA_RUNTIME_NETWORK_OK
                       ? FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE
                       : status;
        }
#else
        iovec buffer{destination, length};
        msghdr message{};
        message.msg_name = &address;
        message.msg_namelen = addressLength;
        message.msg_iov = &buffer;
        message.msg_iovlen = 1;
        const ssize_t result = recvmsg(handle->socket, &message, MSG_TRUNC);
        if (result >= 0) {
            *count = std::min<size_t>(static_cast<size_t>(result), length);
            status = ReadNativeAddress(
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<NativeSocketLength>(message.msg_namelen), family,
                high, low, port);
            if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
            return static_cast<size_t>(result) > length ||
                           (message.msg_flags & MSG_TRUNC) != 0
                       ? FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE
                       : FLECHA_RUNTIME_NETWORK_OK;
        }
        const int error = LastSocketError();
#endif
        if (SocketErrorInterrupted(error) || SocketErrorWouldBlock(error))
            continue;
        return MapSocketError(error);
    }
}

int32_t flecha_runtime_v1_udp_socket_receive(void* opaque, uint8_t* destination,
                                             size_t length, size_t* count) {
    if (count != nullptr) *count = 0;
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (count == nullptr || (destination == nullptr && length != 0))
        return FLECHA_RUNTIME_NETWORK_FAILED;
#ifdef _WIN32
    if (length > static_cast<size_t>(ULONG_MAX))
#else
    if (length > static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
#endif
        return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;

    const Deadline deadline = MakeDeadline(handle->receiveTimeout);
    for (;;) {
        int32_t status = WaitForSocket(handle->socket, false, deadline);
        if (status != FLECHA_RUNTIME_NETWORK_OK) return status;
#ifdef _WIN32
        WSABUF buffer{};
        buffer.buf = reinterpret_cast<char*>(destination);
        buffer.len = static_cast<ULONG>(length);
        DWORD received = 0;
        DWORD flags = 0;
        const int result = WSARecv(handle->socket, &buffer, 1, &received,
                                   &flags, nullptr, nullptr);
        if (result == 0) {
            *count = static_cast<size_t>(received);
            return FLECHA_RUNTIME_NETWORK_OK;
        }
        const int error = LastSocketError();
        if (error == WSAEMSGSIZE) {
            *count = static_cast<size_t>(received);
            return FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE;
        }
#else
        iovec buffer{destination, length};
        msghdr message{};
        message.msg_iov = &buffer;
        message.msg_iovlen = 1;
        const ssize_t result = recvmsg(handle->socket, &message, MSG_TRUNC);
        if (result >= 0) {
            *count = std::min<size_t>(static_cast<size_t>(result), length);
            return static_cast<size_t>(result) > length ||
                           (message.msg_flags & MSG_TRUNC) != 0
                       ? FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE
                       : FLECHA_RUNTIME_NETWORK_OK;
        }
        const int error = LastSocketError();
#endif
        if (SocketErrorInterrupted(error) || SocketErrorWouldBlock(error))
            continue;
        return MapSocketError(error);
    }
}

int32_t flecha_runtime_v1_udp_socket_set_timeout(void* opaque,
                                                 uint8_t direction,
                                                 uint8_t enabled,
                                                 uint64_t seconds,
                                                 uint32_t nanoseconds) {
    UdpHandle* handle = GetUdpHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_NETWORK_CLOSED;
    if (direction > 1 || enabled > 1 || nanoseconds >= 1000000000)
        return FLECHA_RUNTIME_NETWORK_FAILED;
    TcpTimeout& timeout =
        direction == 0 ? handle->receiveTimeout : handle->sendTimeout;
    timeout = TcpTimeout{enabled != 0, seconds, nanoseconds};
    return FLECHA_RUNTIME_NETWORK_OK;
}

int32_t flecha_runtime_v1_udp_socket_local_address(void* handle,
                                                   uint8_t* family,
                                                   uint64_t* high,
                                                   uint64_t* low,
                                                   uint16_t* port) {
    return UdpSocketAddress(handle, false, family, high, low, port);
}

int32_t flecha_runtime_v1_udp_socket_peer_address(void* handle, uint8_t* family,
                                                  uint64_t* high, uint64_t* low,
                                                  uint16_t* port) {
    return UdpSocketAddress(handle, true, family, high, low, port);
}

int32_t flecha_runtime_v1_udp_socket_close(void* handle) {
    return CloseUdpHandle(handle);
}

}  // extern "C"
