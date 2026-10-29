# Text utilities

Flecha's standard text modules build on the byte-oriented `string` and
`stringv` types described in [strings](string.md). They do not change string
indexing or length semantics: both continue to count bytes.

## Numeric conversion

The `strings` module provides fallible integer and floating-point conversion:

```flecha
integer, integerError := strings::stringv_to_int<i64>("-42");
decimal, decimalError := strings::stringv_to_float<f64>("1.25e2");

integerText, integerFormatError := strings::int_to_string(integer);
decimalText, decimalFormatError := strings::float_to_string(decimal);
```

Parsing consumes the complete input and never skips whitespace. Integer
conversion reports empty input, invalid syntax, and target-width overflow.
Floating-point conversion accepts decimal notation plus the canonical
non-finite spellings `nan`, `inf`, and `-inf`; overflow and underflow report
`ParseError.Overflow`. The runtime copies the length-delimited input before
calling the platform conversion boundary, so allocation failure is reported as
`AllocationError.OutOfMemory`.

`format_integer` and `format_float` write into caller-provided byte slices.
Failure leaves the destination unchanged. Floating-point formatting is
locale-independent and emits the shortest representation that parses back to
the same `f32` or `f64` value. It preserves negative zero and uses lowercase
non-finite spellings.

## Incremental construction

`StringBuilder` accumulates byte-oriented text while reporting allocation
failure explicitly. Alongside raw text, bytes, Unicode code points, integers,
and booleans, it can append deterministic floating-point text:

```flecha
builder := strings::StringBuilder::init();
err := builder.append("value=");
if err == none {
    err = builder.appendFloat<f64>(1.25);
}
```

Failed numeric conversion or allocation preserves the builder's existing
contents.
