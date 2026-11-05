#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#include <windows.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#else
#include <time.h>
#endif

namespace {

constexpr uint64_t NANOSECONDS_PER_SECOND = 1000000000;

template <typename Seconds>
int32_t ClearAndValidate(Seconds* seconds, uint32_t* nanoseconds) {
    if (seconds != nullptr) *seconds = 0;
    if (nanoseconds != nullptr) *nanoseconds = 0;
    return seconds != nullptr && nanoseconds != nullptr
               ? FLECHA_RUNTIME_TIME_OK
               : FLECHA_RUNTIME_TIME_FAILED;
}

#ifdef _WIN32

constexpr uint64_t WINDOWS_TICKS_PER_SECOND = 10000000;
constexpr uint64_t WINDOWS_TO_UNIX_EPOCH_TICKS = 116444736000000000;

uint64_t ScaleCounterRemainder(uint64_t remainder, uint64_t frequency) {
#ifdef _MSC_VER
    uint64_t high = 0;
    const uint64_t low = _umul128(remainder, NANOSECONDS_PER_SECOND, &high);
    uint64_t ignoredRemainder = 0;
    return _udiv128(high, low, frequency, &ignoredRemainder);
#else
    using Wide = unsigned __int128;
    return static_cast<uint64_t>(static_cast<Wide>(remainder) *
                                 NANOSECONDS_PER_SECOND / frequency);
#endif
}

int32_t MonotonicNow(uint64_t* seconds, uint32_t* nanoseconds) {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (QueryPerformanceFrequency(&frequency) == 0 || frequency.QuadPart <= 0)
        return FLECHA_RUNTIME_TIME_UNAVAILABLE;
    if (QueryPerformanceCounter(&counter) == 0 || counter.QuadPart < 0)
        return FLECHA_RUNTIME_TIME_FAILED;

    const uint64_t unsignedCounter = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t unsignedFrequency =
        static_cast<uint64_t>(frequency.QuadPart);
    *seconds = unsignedCounter / unsignedFrequency;
    *nanoseconds = static_cast<uint32_t>(ScaleCounterRemainder(
        unsignedCounter % unsignedFrequency, unsignedFrequency));
    return FLECHA_RUNTIME_TIME_OK;
}

int32_t WallNow(int64_t* unixSeconds, uint32_t* nanoseconds) {
    FILETIME fileTime{};
    GetSystemTimePreciseAsFileTime(&fileTime);
    const uint64_t ticks =
        (static_cast<uint64_t>(fileTime.dwHighDateTime) << 32) |
        static_cast<uint64_t>(fileTime.dwLowDateTime);

    if (ticks >= WINDOWS_TO_UNIX_EPOCH_TICKS) {
        const uint64_t delta = ticks - WINDOWS_TO_UNIX_EPOCH_TICKS;
        const uint64_t seconds = delta / WINDOWS_TICKS_PER_SECOND;
        if (seconds >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return FLECHA_RUNTIME_TIME_OUT_OF_RANGE;
        *unixSeconds = static_cast<int64_t>(seconds);
        *nanoseconds =
            static_cast<uint32_t>((delta % WINDOWS_TICKS_PER_SECOND) * 100);
        return FLECHA_RUNTIME_TIME_OK;
    }

    const uint64_t distance = WINDOWS_TO_UNIX_EPOCH_TICKS - ticks;
    const uint64_t wholeSeconds = distance / WINDOWS_TICKS_PER_SECOND;
    const uint64_t remainder = distance % WINDOWS_TICKS_PER_SECOND;
    *unixSeconds = -static_cast<int64_t>(wholeSeconds);
    if (remainder != 0) {
        --*unixSeconds;
        *nanoseconds =
            static_cast<uint32_t>((WINDOWS_TICKS_PER_SECOND - remainder) * 100);
    }
    return FLECHA_RUNTIME_TIME_OK;
}

int32_t Sleep(uint64_t seconds, uint32_t nanoseconds) {
    const uint64_t subsecondTicks =
        (static_cast<uint64_t>(nanoseconds) + 99) / 100;
    constexpr uint64_t MAXIMUM_TICKS =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (seconds > (MAXIMUM_TICKS - subsecondTicks) / WINDOWS_TICKS_PER_SECOND)
        return FLECHA_RUNTIME_TIME_OUT_OF_RANGE;

    const uint64_t ticks = seconds * WINDOWS_TICKS_PER_SECOND + subsecondTicks;
    if (ticks == 0) return FLECHA_RUNTIME_TIME_OK;

    HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    if (timer == nullptr) return FLECHA_RUNTIME_TIME_FAILED;

    LARGE_INTEGER dueTime{};
    dueTime.QuadPart = -static_cast<int64_t>(ticks);
    if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE) == 0) {
        CloseHandle(timer);
        return FLECHA_RUNTIME_TIME_FAILED;
    }

    const DWORD result = WaitForSingleObject(timer, INFINITE);
    const BOOL closed = CloseHandle(timer);
    return result == WAIT_OBJECT_0 && closed != 0 ? FLECHA_RUNTIME_TIME_OK
                                                  : FLECHA_RUNTIME_TIME_FAILED;
}

#else

int32_t MapClockError(int error) {
    switch (error) {
#ifdef EOVERFLOW
        case EOVERFLOW:
            return FLECHA_RUNTIME_TIME_OUT_OF_RANGE;
#endif
        case EINVAL:
#ifdef ENOSYS
        case ENOSYS:
#endif
            return FLECHA_RUNTIME_TIME_UNAVAILABLE;
        default:
            return FLECHA_RUNTIME_TIME_FAILED;
    }
}

template <typename Seconds>
bool AssignSeconds(time_t value, Seconds* output) {
    if (!std::in_range<Seconds>(value)) return false;
    *output = static_cast<Seconds>(value);
    return true;
}

template <typename Seconds>
int32_t ClockNow(clockid_t clock, Seconds* seconds, uint32_t* nanoseconds) {
    timespec value{};
    errno = 0;
    if (clock_gettime(clock, &value) != 0) return MapClockError(errno);
    if (value.tv_nsec < 0 ||
        value.tv_nsec >= static_cast<long>(NANOSECONDS_PER_SECOND) ||
        !AssignSeconds(value.tv_sec, seconds))
        return FLECHA_RUNTIME_TIME_OUT_OF_RANGE;

    *nanoseconds = static_cast<uint32_t>(value.tv_nsec);
    return FLECHA_RUNTIME_TIME_OK;
}

int32_t Sleep(uint64_t seconds, uint32_t nanoseconds) {
    if (!std::in_range<time_t>(seconds))
        return FLECHA_RUNTIME_TIME_OUT_OF_RANGE;

    timespec request{
        static_cast<time_t>(seconds),
        static_cast<long>(nanoseconds),
    };
    for (;;) {
        timespec remaining{};
        if (nanosleep(&request, &remaining) == 0) return FLECHA_RUNTIME_TIME_OK;
        if (errno != EINTR) return FLECHA_RUNTIME_TIME_FAILED;
        request = remaining;
    }
}

#endif

}  // namespace

extern "C" {

int32_t flecha_runtime_v1_monotonic_clock_now(uint64_t* seconds,
                                              uint32_t* nanoseconds) {
    const int32_t validation = ClearAndValidate(seconds, nanoseconds);
    if (validation != FLECHA_RUNTIME_TIME_OK) return validation;

#ifdef _WIN32
    return MonotonicNow(seconds, nanoseconds);
#else
    return ClockNow(CLOCK_MONOTONIC, seconds, nanoseconds);
#endif
}

int32_t flecha_runtime_v1_wall_clock_now(int64_t* unixSeconds,
                                         uint32_t* nanoseconds) {
    const int32_t validation = ClearAndValidate(unixSeconds, nanoseconds);
    if (validation != FLECHA_RUNTIME_TIME_OK) return validation;

#ifdef _WIN32
    return WallNow(unixSeconds, nanoseconds);
#else
    return ClockNow(CLOCK_REALTIME, unixSeconds, nanoseconds);
#endif
}

int32_t flecha_runtime_v1_sleep(uint64_t seconds, uint32_t nanoseconds) {
    if (nanoseconds >= NANOSECONDS_PER_SECOND)
        return FLECHA_RUNTIME_TIME_INVALID_DURATION;
    if (seconds == 0 && nanoseconds == 0) return FLECHA_RUNTIME_TIME_OK;
    return Sleep(seconds, nanoseconds);
}

}  // extern "C"
