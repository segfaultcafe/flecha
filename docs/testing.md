# Testing assertions

The exact v0.3 declarations and focused tests are indexed in the
[standard-library reference](standard_library.md#std-tests-testing).

Flecha test modules receive a `testing::UnitTest` and record checks through
`lib::std::tests::asserts`:

```flecha
module calculator_test;

@imports
    :: root::calculator
    :: lib::std::tests::asserts
    :: lib::std::tests::testing;

test adds_two_values(&testing::UnitTest t) {
    i32 actual = calculator::add(20, 22);
    i32 expected = 42;
    asserts::numeric_equal<i32>(
        t,
        &actual,
        &expected,
        "20 + 22 is 42"
    );
}
```

Run one test module with `flecha test path/to/calculator_test.fl`. Test files,
module pairing, and discovery follow the rules described in the
[v0.2 testing design](release_0_2.md#test-modules).

Every assertion records exactly one assertion. A failed assertion also records
one failure and appends its details to the test's deferred output. The runner
prints that output with the test result; successful assertions do not format
diagnostic values or allocate diagnostic storage.

## General assertions

The general helpers cover Boolean, generic value, type-strict, default-value,
pointer, and error checks:

```flecha
asserts::is_true(t, ready, "the operation completed");
asserts::is_false(t, failed, "the operation did not fail");
asserts::equal<i32>(t, &expected, &actual, "values match");
asserts::not_equal<i32>(t, &unexpected, &actual, "values differ");
asserts::type_equal<i32, i32>(t, &expected, &actual, "types and values match");
asserts::type_not_equal<i32, u32>(t, &signedValue, &unsignedValue, "types differ");
asserts::is_none<error>(t, &failure, "no error was returned");
asserts::is_null(t, &pointer, "no allocation was returned");
asserts::has_error(t, failure, "an error was returned");
```

`equal`, `not_equal`, `type_equal`, and `type_not_equal` keep their established
argument order: expected or unexpected first, then actual. They work for any
compatible type and report the expression on failure. Use the specialized
numeric, string, and byte helpers below when a failure should include values or
mismatch context.

## Numeric comparisons

Numeric assertions take actual first and expected or unexpected second:

```flecha
asserts::less<i32>(t, &actual, &limit, "value is below the limit");
asserts::less_or_equal<i32>(t, &actual, &limit, "value is at most the limit");
asserts::greater<i32>(t, &actual, &minimum, "value is above the minimum");
asserts::greater_or_equal<i32>(t, &actual, &minimum, "value meets the minimum");
asserts::numeric_equal<i32>(t, &actual, &expected, "values match");
asserts::numeric_not_equal<i32>(t, &actual, &unexpected, "values differ");
```

All numeric types are supported. A failed comparison reports both values, for
example `actual < expected (actual=5, expected=4)`.

## Strings and bytes

Use `string_equal` and `string_not_equal` for byte-oriented string comparison:

```flecha
asserts::string_equal(t, actualText, expectedText, "text matches");
asserts::string_not_equal(t, actualText, rejectedText, "text differs");
```

Failure output quotes both strings and escapes double quotes, backslashes,
newline, carriage return, tab, NUL, and other ASCII control bytes. Diagnostics
show strings up to 64 bytes in full. Longer strings retain the first and last
32 bytes and include the omitted byte count between them. This limit applies to
diagnostics only; the comparison always examines the complete strings.

Use the byte-slice helpers for binary data:

```flecha
asserts::bytes_equal(t, actualBytes, expectedBytes, "payload matches");
asserts::bytes_not_equal(t, actualBytes, rejectedBytes, "payload differs");
```

`bytes_equal` reports differing lengths or the first mismatching index and byte
values. `bytes_not_equal` succeeds on the first length or content difference;
if the slices are identical, its failure reports their common length.

## Error families and variants

`has_error_family` checks for any non-`none` member of one named error family.
The family is the explicit generic specialization:

```flecha
error ParseError {
    MissingDigit,
    OutOfRange,
}

ParseError failure = parse(input);
asserts::has_error_family<ParseError>(
    t,
    failure,
    "parsing returns a ParseError"
);
```

The specialization must be a named family. Built-in generic `error` does not
identify an expected family and therefore does not satisfy this assertion.
The typed parameter prevents values from another named family from being
passed accidentally.

`has_error_variant` checks both family and variant identity after widening to
generic error:

```flecha
asserts::has_error_variant(
    t,
    failure,
    ParseError.OutOfRange,
    "the value is outside the accepted range"
);
```

The expected value must not be `none`. A different variant, a variant from a
different family, or `none` fails the assertion.

## Allocation failures

Value-rich diagnostics allocate only after an assertion fails. If diagnostic
formatting cannot allocate, the assertion still records exactly one assertion
and one failure, preserves the caller's message, and falls back to its stable
expression label. Allocation failure never turns a failed assertion into a
successful one and never exposes a partial diagnostic.

## Temporary fixtures and deterministic data

`lib::std::tests::fixtures` provides test support implemented in Flecha. A
`TemporaryDirectory` owns an isolated directory tree and removes it recursively
when it leaves scope. Moving the value transfers cleanup responsibility. Call
`remove()` for explicit cleanup; if cleanup fails, the fixture retains its path
so the cause can be repaired and cleanup retried.

Use `childPath` to allocate a normalized path inside the fixture. It rejects
empty, absolute, volume-qualified, NUL-containing, and escaping parent paths:

```flecha
temporary, creationError := fixtures::TemporaryDirectory::create();
asserts::is_none<error>(t, &creationError, "the fixture is created");
if creationError != none {
    return;
}

inputPath, pathError := temporary.childPath("inputs/example.txt");
asserts::is_none<error>(t, &pathError, "the input path is safe");
```

`RandomData::fromSeed` creates reproducible non-cryptographic input. It exposes
integer, bounded-integer, unit-float, Boolean, byte-fill, and shuffle operations.
Retain the value returned by `seed()`, or call `logSeed(t)` to append
`random seed: N` to deferred test output so a failing run can be reproduced.

## Golden files

`lib::std::tests::golden` compares text or arbitrary bytes with a checked-in
golden file and records the result in an existing `UnitTest`:

```flecha
verifyError := golden::text(
    t,
    rendered.view(),
    "tests/golden/rendered.txt",
    "rendered output matches"
);
```

Verification is the default and never creates or changes a file. Updating is a
per-call, explicit opt-in:

```flecha
updateError := golden::text(
    t,
    rendered.view(),
    "tests/golden/rendered.txt",
    "replace the rendered golden",
    golden::Mode.Update
);
```

Use `golden::bytes` for intentionally binary data. Text is also byte-oriented;
invalid UTF-8 and control bytes compare exactly and are escaped in mismatch
diagnostics. A mismatch is recorded as an assertion failure and returns `none`;
file I/O and malformed-path failures are both recorded and returned to the
caller. Every golden operation records exactly one assertion.

## Deterministic benchmarks

`lib::std::tests::benchmark` measures in-process workloads with the monotonic
clock. Configuration is explicit and validated:

```flecha
config, configError := benchmark::Config::create(
    10,   // untimed warmup iterations
    100,  // workload iterations in each timed sample
    20    // timed samples
);
```

Zero warmup iterations are valid. Zero iterations per sample return
`BenchmarkError.InvalidIterations`, and zero samples return
`BenchmarkError.InvalidSamples`. Benchmark names are stable machine keys. They
must be nonempty ASCII letters, digits, `.`, `_`, `-`, `/`, or `:`; other bytes
return `BenchmarkError.InvalidName`.

### Library workload

Pass an error-returning reusable callback to `run`. Warmup calls are never
recorded as samples:

```flecha
result, runError := benchmark::run(
    "std.bytes.equal",
    config,
    [&left, &right]() => (error) {
        bool same = sequence::equal(left[:], right[:]);
        if !same {
            return WorkError.UnexpectedMismatch;
        }
        return none;
    }
);
```

Each sample measures the complete configured group of workload iterations.
`minimumDuration`, `medianDuration`, `meanDuration`, and `maximumDuration`
therefore describe whole samples. The runner reports integer nanoseconds. An
even median uses the integer midpoint and the mean truncates a remainder below
one nanosecond; it does not print decimal digits that the clock cannot justify.

### Allocation-sensitive workload

Allocation can be included deliberately in the timed callback:

```flecha
result, runError := benchmark::run(
    "std.array.reserve",
    config,
    () => (error) {
        values := collections::Array<u8>::init();
        return values.reserve(4096);
    }
);
```

When allocation or input preparation must be excluded, use `run_with_hooks`.
Its setup and teardown callbacks surround each timed sample without joining the
measured interval. A nonzero warmup count forms one additional untimed
setup/work/teardown phase. Teardown still runs after a workload or clock error;
the earlier error is returned.

### Compiler and CI workloads

Host tooling can time external compiler commands and pass the complete sample
durations to `summarize`. This is useful before Flecha process execution is
available to an in-process benchmark:

```flecha
[5]u64 compilerCheckNanoseconds = [
    82100000, 80400000, 80900000, 81700000, 80600000
];
compilerConfig, configError := benchmark::Config::create(0, 1, 5);
result, summaryError := benchmark::summarize(
    "compiler.check",
    compilerConfig,
    compilerCheckNanoseconds[:]
);
```

`summary()` produces a deterministic human-readable line. `machineLine()`
produces one JSON object without a trailing newline. Its format identifier is
`flecha-benchmark-v1`; keys have a fixed order and all measurements are integer
nanoseconds:

```json
{"format":"flecha-benchmark-v1","name":"compiler.check","warmup_iterations":0,"iterations_per_sample":1,"sample_count":5,"sample_min_ns":80400000,"sample_median_ns":80900000,"sample_mean_ns":81140000,"sample_max_ns":82100000}
```

The runner does not subtract an estimated clock, loop, or indirect-call cost.
A no-op benchmark measures that floor on the current host. Workloads close to
that floor should increase their iterations per sample rather than treating a
subtracted estimate as real precision. CI should compare versioned workloads
under controlled hosts and reviewed tolerances; one sample is not a regression
gate by itself.
