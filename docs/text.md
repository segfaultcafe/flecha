# Text utilities

The exact v0.3 declarations and focused tests are indexed in the
[standard-library reference](standard_library.md#std-text-strings).

Flecha's standard text modules build on the byte-oriented `string` and
`stringv` types described in [strings](string.md). They do not change string
indexing or length semantics: both continue to count bytes.

## ASCII digit conversion

The `ascii` module converts one ASCII decimal digit to its `u8` numeric value:

```flecha
digit, digitError := ascii::char_to_int('7'); // 7, none
```

Characters outside `'0'` through `'9'` return `none` with
`ParseError.InvalidSyntax`. The helper accepts exactly one byte-oriented
`char`; it does not parse Unicode digits or other radices.

## UTF-8 boundaries and reverse decoding

The `utf8` module can inspect byte boundaries and decode backward without
allocating:

```flecha
bool boundary = utf8::is_boundary(text, offset);
codePoint, startOffset, decodingError :=
    utf8::decode_previous(text, exclusiveEndOffset);
```

`is_boundary` accepts offsets from zero through the byte length. Zero and the
end are boundaries; an interior offset is a boundary when its byte is not a
UTF-8 continuation byte. It returns `false` beyond the input. This is a
structural inspection operation and does not validate the complete string.

`decode_previous` requires a nonzero, in-range boundary and returns the scalar
value immediately before it plus that sequence's starting byte offset. Zero,
out-of-range offsets, and offsets inside a valid sequence report
`Utf8Error.InvalidOffset`. Truncated or malformed sequences report the same
`Utf8Error.Truncated` or `Utf8Error.InvalidEncoding` result as forward
`decode`. Boundary inspection and reverse decoding perform no allocation or
host calls.

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

text, copyError := builder.toString();
```

Failed numeric conversion or allocation preserves the builder's existing
contents. `view()` borrows the accumulated bytes without allocating, while
`toString()` copies them into an independent owning string and reports
`AllocationError.OutOfMemory`.

When allocation failure is intentionally unrecoverable, a crashable caller can
select the paired method:

```flecha
string finish!(&const strings::StringBuilder builder) {
    return builder.toString!();
}
```

`toString!()` returns the same owning copy directly. If the copy cannot be
allocated, it emits the standard crash report instead of returning an error.
