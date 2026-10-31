# Time

Flecha separates amounts of time, monotonic observations, and wall-clock
timestamps. These values have different domains and are not interchangeable:

- `Duration` is a nonnegative amount of time, independent of any clock.
- `Instant` is an opaque observation of a monotonic clock and is used to
  measure elapsed time within one program execution.
- `Timestamp` is a wall-clock observation relative to the Unix epoch and is
  used when a value must be stored or exchanged.

Wall-clock time can move forward or backward when the host clock is adjusted.
Monotonic time does not move backward between successful observations in one
program execution. Neither clock promises nanosecond accuracy merely because
its result contains a nanosecond component.

## Module ownership

The public implementation lives under `std/src/time`:

```text
std/src/time/duration.fl       Duration and all duration arithmetic
std/src/time/clock.fl          Instant, Timestamp, clock queries, and sleep
std/src/time/errors.fl         stable time-domain errors
std/src/time/runtime.fl        private declarations of the native ABI
std/src/time/*_test.fl         Flecha contract tests
```

`duration.fl` is implemented entirely in Flecha. `clock.fl` performs portable
arithmetic in Flecha and enters unsafe code only to query a clock or block the
current thread. Native host integration lives in `src/runtime/time.cpp`, with
declarations added to `src/include/runtime/runtime_abi.h` and the runtime build.
`std/src/std.fl` imports `duration`, `clock`, and `errors as time_errors` so the
installed standard-library package discovers and checks the complete surface.

There is no public platform module, tick type, clock frequency, or native
handle. Platform units are converted to normalized seconds and nanoseconds
before they cross the runtime ABI.

## Duration

`Duration` has the logical representation `(seconds: u64, nanoseconds: u32)`.
It is always normalized: `nanoseconds` is less than `1_000_000_000`. Its fields
are private so an invalid value cannot be constructed in safe code. The zero
value is a valid zero duration.

The initial public surface is:

```flecha
export structure Duration {
    private u64 seconds;

    private u32 nanoseconds;
};

Duration Duration::zero();
(Duration, error) Duration::fromParts(u64 seconds, u32 nanoseconds);
Duration Duration::fromSeconds(u64 value);
Duration Duration::fromMilliseconds(u64 value);
Duration Duration::fromMicroseconds(u64 value);
Duration Duration::fromNanoseconds(u64 value);

u64 Duration::wholeSeconds(&const self);
u32 Duration::subsecondNanoseconds(&const self);
(u64, error) Duration::totalMilliseconds(&const self);
(u64, error) Duration::totalMicroseconds(&const self);
(u64, error) Duration::totalNanoseconds(&const self);

bool Duration::isZero(&const self);
bool Duration::lessThan(&const self, Duration other);
bool Duration::lessOrEqual(&const self, Duration other);
bool Duration::greaterThan(&const self, Duration other);
bool Duration::greaterOrEqual(&const self, Duration other);
(Duration, error) Duration::checkedAdd(&const self, Duration other);
(Duration, error) Duration::checkedSub(&const self, Duration other);
(Duration, error) Duration::checkedMul(&const self, u64 factor);
```

`fromParts` rejects a nanosecond component outside the normalized range with
`TimeError.InvalidNanoseconds`; it does not silently carry that component into
seconds. Unit constructors split their input into normalized fields and cannot
overflow because their input is itself a `u64` count of the smaller unit.

The `total*` conversions return the number of complete units, truncating only
the sub-unit remainder. They report `TimeError.Overflow` when the complete
count cannot fit in `u64`. Checked addition and multiplication also report
`TimeError.Overflow`. Checked subtraction reports
`TimeError.NegativeResult` and returns zero when the right operand is greater;
no duration operation wraps or saturates implicitly. Comparisons order first by
seconds and then by the subsecond component.

No floating-point constructors or conversions are part of the initial API.
Callers that perform a rounding policy do so explicitly before constructing a
duration.

## Monotonic clock

`Instant` stores a normalized runtime observation but keeps both fields
private. Application code cannot obtain or depend on its numeric origin. An
instant is meaningful only within the program execution that produced it; it
must not be persisted, sent to another process, or interpreted as a timestamp.

```flecha
export structure Instant {
    private u64 seconds;

    private u32 nanoseconds;

    private bool valid;
};

(Instant, error) Instant::now();

bool Instant::isValid(&const self);
(Duration, error) Instant::durationSince(
    &const self,
    Instant earlier
);
(Duration, error) Instant::elapsed(&const self);
(Instant, error) Instant::checkedAdd(&const self, Duration duration);
(Instant, error) Instant::checkedSub(&const self, Duration duration);
```

The default value is an invalid sentinel rather than a clock observation;
operations that require an observation report `TimeError.InvalidInstant`.
`Instant::now` returns `TimeError.ClockUnavailable` when the host cannot
provide a monotonic observation. Successful observations from one execution
are nondecreasing; equal consecutive observations are valid. The contract does
not specify whether time spent in system suspend contributes to elapsed time,
because supported hosts differ.

`durationSince` and `elapsed` never use the wall clock. Passing a later instant
as `earlier` reports `TimeError.NegativeResult`. Instant arithmetic reports
`TimeError.Overflow` or `TimeError.NegativeResult` rather than wrapping.

## Wall clock

`Timestamp` represents Unix time as a signed seconds field and a normalized
nonnegative nanosecond component. Using floor-style normalization allows times
before 1970: for example, half a second before the epoch is represented as
`(-1, 500_000_000)`. Unix timestamps do not represent leap seconds.

```flecha
export structure Timestamp {
    private i64 unixSeconds;

    private u32 nanoseconds;
};

(Timestamp, error) Timestamp::now();

(Timestamp, error) Timestamp::fromUnixParts(
    i64 seconds,
    u32 nanoseconds
);
i64 Timestamp::unixSeconds(&const self);
u32 Timestamp::subsecondNanoseconds(&const self);
(Duration, error) Timestamp::durationSince(
    &const self,
    Timestamp earlier
);
(Timestamp, error) Timestamp::checkedAdd(
    &const self,
    Duration duration
);
(Timestamp, error) Timestamp::checkedSub(
    &const self,
    Duration duration
);
```

`Timestamp::now` can observe a value earlier than a previous successful call. It
reports `TimeError.ClockUnavailable` when the host query fails and
`TimeError.TimestampOutOfRange` when the host value cannot be represented.
`fromUnixParts` rejects a nanosecond component outside the normalized range.
Timestamp arithmetic is checked and never consults the current wall clock.

## Blocking sleep

`sleep` belongs to `std::time::clock` because it is the only initial blocking
time operation and shares the same narrow runtime boundary:

```flecha
// Blocks the current thread for at least duration unless it returns an error.
export error sleep(Duration duration);
```

A zero duration succeeds without entering the runtime. A successful nonzero
call means the requested monotonic interval has elapsed. Scheduling may make
the call return later than requested. Wall-clock adjustments do not shorten or
extend the requested interval.

On hosts where a signal or equivalent event interrupts the native wait, the
runtime resumes the unelapsed remainder. Interruption is therefore never
reported as successful completion of a shorter sleep. If the duration cannot
be represented by the host wait primitive, `sleep` returns
`TimeError.DurationOutOfRange` before blocking. Other host failures return
`TimeError.SleepFailed`. The initial operation has no cancellation contract
and blocks the current operating-system thread, not merely a Flecha task.

## Errors

The time modules expose one stable family:

```flecha
export error TimeError {
    // A seconds-plus-nanoseconds value is not normalized.
    InvalidNanoseconds,

    // A portable conversion or arithmetic result is not representable.
    Overflow,

    // Subtraction would produce a negative duration or instant.
    NegativeResult,

    // An instant was not produced by the monotonic clock in this execution.
    InvalidInstant,

    // The requested host clock could not provide an observation.
    ClockUnavailable,

    // A wall-clock result cannot be represented by Timestamp.
    TimestampOutOfRange,

    // A valid Duration cannot be represented by the host sleep primitive.
    DurationOutOfRange,

    // The host wait failed for another reason.
    SleepFailed,
}
```

Clock and sleep APIs perform no allocation. They therefore never return an
allocation error.

## Runtime ABI

Version 1 gains three additive operations and one stable status table:

```c
typedef enum FlechaRuntimeTimeStatus {
    FLECHA_RUNTIME_TIME_OK = 0,
    FLECHA_RUNTIME_TIME_UNAVAILABLE = 1,
    FLECHA_RUNTIME_TIME_OUT_OF_RANGE = 2,
    FLECHA_RUNTIME_TIME_INVALID_DURATION = 3,
    FLECHA_RUNTIME_TIME_FAILED = 4,
} FlechaRuntimeTimeStatus;

int32_t flecha_runtime_v1_monotonic_clock_now(uint64_t* seconds,
                                               uint32_t* nanoseconds);
int32_t flecha_runtime_v1_wall_clock_now(int64_t* unix_seconds,
                                         uint32_t* nanoseconds);
int32_t flecha_runtime_v1_sleep(uint64_t seconds, uint32_t nanoseconds);
```

Both query operations clear every non-null output before doing fallible work,
require both output pointers, and leave both outputs zero on failure. A
successful result always has `nanoseconds < 1_000_000_000`.

The sleep operation rejects a nanosecond value outside that range with
`FLECHA_RUNTIME_TIME_INVALID_DURATION`. It checks conversion to the host's
native range before waiting. Host interruptions are retried with the remaining
interval inside the runtime; the ABI does not expose platform signal codes or
partial native waits.

The safe wrapper maps the shared table in the context of each operation:

| Operation | Runtime status | Flecha error |
| --- | --- | --- |
| `Instant::now` | unavailable, out of range, or failed | `TimeError.ClockUnavailable` |
| `Timestamp::now` | unavailable or failed | `TimeError.ClockUnavailable` |
| `Timestamp::now` | out of range | `TimeError.TimestampOutOfRange` |
| `sleep` | out of range | `TimeError.DurationOutOfRange` |
| `sleep` | invalid duration | `TimeError.InvalidNanoseconds` |
| `sleep` | unavailable or failed | `TimeError.SleepFailed` |

Statuses not meaningful for an operation are treated as that operation's
generic failure. The invalid-duration result is defensive ABI validation; a
safe `Duration` can never cause it.

On POSIX targets, the implementation uses `CLOCK_MONOTONIC` and
`CLOCK_REALTIME` and a relative sleep primitive that resumes after `EINTR`. On
Windows, it uses `QueryPerformanceCounter` for monotonic observations, precise
system time converted from the Windows epoch for wall time, and a relative
wait. Every conversion checks range and normalizes before returning. No C++
type, platform tick, clock identifier, or native error code crosses the ABI.

The operations are safe to call concurrently from multiple threads. They do
not mutate the process wall clock or transfer ownership of any resource.

## Delivery order

Time is delivered as three independently reviewable changes:

1. `duration.fl`, its error variants, and pure Flecha boundary tests;
2. `Instant`, `Timestamp`, the two runtime clock queries, and native plus
   end-to-end clock tests; and
3. blocking `sleep`, host interruption and range handling, and timing tests.

The clock change does not need sleep, and duration does not need any runtime
symbol. This keeps portable arithmetic review separate from platform behavior.

## Verification gate

The duration tests cover zero and maximum values, every constructor boundary,
subsecond truncation, total-unit overflow, carry and borrow across a second,
negative subtraction, multiplication overflow, equality, and normalization.

Native runtime tests cover null outputs, cleared outputs on every failure,
normalized successful results, nondecreasing monotonic observations, invalid
sleep nanoseconds, host-range rejection, zero sleep, and interruption retry on
POSIX. Tests must not require consecutive observations to differ or assume a
particular clock resolution.

End-to-end Flecha tests verify that elapsed measurement uses the monotonic
clock, wall timestamps expose normalized Unix parts, a short sleep does not
return successfully before its requested monotonic interval, and all new
symbols link from an installed or relocated release package. Sleep tests use a
generous upper timeout only to detect hangs; scheduler delay is not a contract
failure.

Secure randomness and deterministic generation now follow this gate through
the contracts in [`random.md`](random.md). Child-process creation, exit status,
and pipes remain the final Stage 5 operating-system work. Blocking networking
depends on these foundations; hashing and security, expanded testing
infrastructure, and the Stage 9 hardening gate follow in the milestone sequence.
