# Randomness

Flecha separates cryptographically secure operating-system randomness from
deterministic general-purpose generation:

- `std::random::secure` fills caller-owned bytes directly from the host secure
  random source.
- `std::random::generator` provides reproducible integers, floating-point
  values, booleans, byte streams, and shuffling.

These APIs are not interchangeable. Use `secure::fill` for keys, salts,
nonces, tokens, and seeds that must be unpredictable. Use `Generator` for
games, simulations, procedural generation, randomized tests, and replayable
workloads. A `Generator` initialized by `fromSecure` is still not a
cryptographic generator.

## Module ownership

The implementation lives under `std/src/random`:

```text
std/src/random/secure.fl          safe operating-system randomness
std/src/random/generator.fl       deterministic generator and algorithms
std/src/random/errors.fl          stable random-domain errors
std/src/random/runtime.fl         private native ABI declaration
std/src/random/*_test.fl          Flecha contract tests
```

Native host integration lives in `src/runtime/random.cpp`. The public ABI
declaration is in `src/include/runtime/runtime_abi.h`. The `std` facade imports
`generator`, `secure`, and `errors as random_errors`.

No public API exposes platform handles, entropy-device paths, native status
codes, or operating-system tick values.

## Secure bytes

The secure API is intentionally narrow:

```flecha
export errors::RandomError fill([]u8 destination);
```

`secure::fill` has the following contract:

- Success fills the complete destination.
- An empty destination succeeds.
- The operation performs no allocation.
- It may block while the operating system initializes its entropy source.
- Calls do not share userspace state and are safe for concurrent use.
- Failure may leave any prefix modified. Callers must discard the complete
  destination after an error.
- The runtime never falls back to a deterministic userspace generator.
- Randomness does not guarantee uniqueness; protocols must still define nonce
  and identifier rules appropriate to their domain.

Example:

```flecha
@imports :: lib::std::random::secure;

[32]u8 key = none;
fillError := secure::fill(key[:]);
if fillError != none {
    return fillError;
}
```

The caller owns `key` before, during, and after the call. On failure, its bytes
must not be inspected or reused as random material.

## Deterministic generator

`Generator` uses xoshiro256** with four private `u64` state words. `fromSeed`
expands one `u64` through SplitMix64. The algorithm, seed expansion, and byte
order are tested with golden vectors so one seed produces the same sequence on
every supported host.

```flecha
export structure Generator {
    private {
        u64 state0;
        u64 state1;
        u64 state2;
        u64 state3;
    }
};

Generator Generator::fromSeed(u64 seed);
(Generator, errors::RandomError) Generator::fromSecure();

T Generator::integer<T: integer>(&self);
(T, errors::RandomError) Generator::integerIn<T: integer>(
    &self,
    T lower,
    T upper
);
T Generator::float<T: floating_point>(&self);
bool Generator::boolean(&self);
void Generator::fill(&self, []u8 destination);
void Generator::shuffle<T>(&self, []T values);
```

Generation operations advance the receiver according to the random output they
consume. Documented no-ops, such as filling an empty slice or shuffling fewer
than two elements, do not advance it. Copying a generator copies its current
state and therefore creates an identical future sequence until the copies are
advanced differently. Callers that require one
ordered stream must not mutate the same generator concurrently.

### Construction

`fromSeed` is infallible and reproducible. Store or report the seed when a
simulation, generated world, or failing randomized test must be replayed.

`fromSecure` requests 256 bits from `secure::fill` and returns its stable error
on failure. It is useful when a caller wants a different general-purpose
sequence on each execution without managing a seed. Secure initialization does
not make later xoshiro output suitable for cryptographic material.

### Integers

`integer<T>` is uniform across every representable value of `T`, including
signed minimum and maximum values.

`integerIn<T>(lower, upper)` is uniform over the half-open interval
`[lower, upper)`. It returns `RandomError.InvalidRange` when `lower >= upper`
and does not advance the generator on that error. Bounded generation uses
rejection sampling rather than plain modulo reduction, avoiding modulo bias.

Generic method arguments may be inferred:

```flecha
rng := generator::Generator::fromSeed(84323);

i32 roomCount;
roomCount, rangeError := rng.integerIn(8, 32);
```

Here both integer arguments and the known first tuple destination provide
`T = i32`. Explicit `rng.integerIn<i32>(8, 32)` remains valid.

### Floating-point values and booleans

`float<f32>` and `float<f64>` return values in `[0, 1)`. The upper endpoint is
never produced. The `f32` specialization uses 24 random bits and `f64` uses 53,
matching their significand precision. The initial API deliberately has no
ranged floating-point operation because endpoint rounding needs its own
contract.

`boolean` returns `false` and `true` with equal probability.

### Byte filling and shuffling

`Generator::fill` writes a deterministic byte stream. Bytes are emitted least
significant first from each generated `u64`, making seeded output independent
of host byte order. An empty destination does not advance the generator.

`shuffle` performs unbiased Fisher-Yates permutation over copyable elements.
Empty and single-element slices are no-ops and do not advance the generator.

## Errors

```flecha
export error RandomError {
    // The host has no supported cryptographically secure random source.
    Unavailable,

    // The host source failed while servicing the request.
    GenerationFailed,

    // A requested interval is empty or reversed.
    InvalidRange,
}
```

The runtime never exposes `errno`, `NTSTATUS`, or another host-specific value.
Unknown runtime failures map to `GenerationFailed`.

## Runtime ABI

Version 1 adds one status table and one operation:

```c
typedef enum FlechaRuntimeRandomStatus {
    FLECHA_RUNTIME_RANDOM_OK = 0,
    FLECHA_RUNTIME_RANDOM_UNAVAILABLE = 1,
    FLECHA_RUNTIME_RANDOM_FAILED = 2,
} FlechaRuntimeRandomStatus;

int32_t flecha_runtime_v1_secure_random_fill(uint8_t* destination,
                                             size_t length);
```

`length == 0` succeeds even when `destination` is null. A null destination with
a nonzero length fails. Success means the complete range was filled. Failure
may occur after a prefix was written.

Linux uses `getrandom` with flags zero, retries `EINTR`, and continues after
short reads. macOS uses `getentropy` in chunks no larger than 256 bytes and
retries interruptions. Windows uses `BCryptGenRandom` with the system-preferred
random source and splits requests at the native length limit. `ENOSYS` maps to
`Unavailable`; other host failures map to `GenerationFailed`.

## Verification gate

Native tests cover empty and invalid destinations plus canaries around a
513-byte fill. Flecha secure tests cover empty, nonempty, and bounded-slice
destinations without probabilistic assertions about the returned bytes.

Generator tests cover published seeded vectors, signed and unsigned integer
ranges, invalid intervals, `f32` and `f64` bounds, booleans, portable byte
order, shuffle preservation, and no-op operations. A deterministic native
harness supplies known secure bytes and injected runtime statuses to verify
`fromSecure`, `Unavailable`, and `GenerationFailed` without depending on a real
operating-system failure.

The initial surface intentionally excludes probability distributions, weighted
selection, ranged floating-point generation, UUID/token formatting, a global
generator, state serialization, stream splitting, and cryptographic
pseudorandom generation.
