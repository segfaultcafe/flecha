# Testing assertions

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
