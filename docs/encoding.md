# Encoding

The exact v0.3 declarations and focused tests are indexed in the
[standard-library reference](standard_library.md#std-encoding-binary).

The `encoding` namespace owns transformations between values and canonical
byte representations. Raw byte storage and sequence operations remain in the
[`bytes` namespace](bytes.md).

## Binary

The `binary` module provides explicit-endian primitives, varints, borrowed
cursors, and adapters for appending encoded values to `bytes::buffer::Buffer`:

```flecha
@imports :: lib::std::encoding::binary;

value, readError := binary::read_little<u32>(source);
writeError := binary::write_big<u64>(destination, value64);

reader := binary::Reader::init(source);
next, nextError := reader.readVarint<u32>();
delta, deltaError := reader.readSignedVarint<i32>();

writer := binary::Writer::init(destination);
appendError := writer.writeLittle<u16>(4660);
signedError := writer.writeSignedVarint<i64>(-42);
```

`read_little`, `read_big`, `write_little`, and `write_big` accept the exported
`fixed_unsigned_integer` type set: `u8`, `u16`, `u32`, and `u64`. `usize` is
excluded because its target-dependent width cannot define canonical data.

`read_varint` and `write_varint` use canonical unsigned base-128 encoding.
`varint_length` returns the encoded width. Truncated input, values outside the
target width, and redundant encodings report `BinaryError` without consuming a
cursor or partially modifying a destination.

`zigzag_encode` and `zigzag_decode` map `i8`, `i16`, `i32`, and `i64` through
the unsigned width corresponding to each signed type. The
`signed_varint_length`, `read_signed_varint`, and `write_signed_varint`
operations then use that mapping with canonical unsigned base-128 bytes. This
is the Protocol Buffers ZigZag mapping: `0`, `-1`, `1`, and `-2` encode as
unsigned values `0`, `1`, `2`, and `3`. Decoding rejects a canonical unsigned
value that exceeds the matching signed type's wire width. Truncated,
overflowing, and redundant encodings preserve the same transactional behavior
as unsigned varints. `isize` is excluded because its width is target-dependent.

`Reader` borrows an immutable source and `Writer` borrows a mutable
destination. Their read and write operations are transactional: failure leaves
the cursor position unchanged. Neither cursor allocates.

The Buffer adapters keep serialization ownership in `encoding::binary` rather
than adding format-specific methods to the raw Buffer:

```flecha
binary::append_little<u32>(&buffer, value);
binary::append_big<u64>(&buffer, wide);
binary::append_varint<u32>(&buffer, count);
binary::append_signed_varint<i32>(&buffer, delta);
```

## Hexadecimal

The `hex` module encodes raw bytes as hexadecimal ASCII and decodes either
letter case. Like Base64, its primary string API allocates the result for the
caller:

```flecha
@imports :: lib::std::encoding::hex;

encoded, encodeError := hex::encode("foobar");
decoded, decodeError := hex::decode("666f6f626172");
```

`encode` emits lowercase characters and `encode_upper` emits uppercase.
`encode_bytes` accepts arbitrary borrowed bytes, and the
`encode_byte_to_byte` and `encode_to_bytes` variants return newly allocated
hexadecimal bytes. The uppercase API provides matching `_bytes`,
`_byte_to_byte`, and `_to_bytes` variants. `decode_from_bytes` returns a
byte-oriented `string`, while `decode_byte_to_byte` and `decode_to_bytes`
return newly allocated binary bytes.

Decoding validates the complete source before allocation and rejects odd
lengths and non-hexadecimal characters. Length overflow and malformed input
report `HexError`; allocation failures are returned through the owning APIs'
general `error` result.

## Base64

The `base64` module implements the standard padded Base64 alphabet from RFC
4648. Its primary string API allocates the result for the caller:

```flecha
@imports :: lib::std::encoding::base64;

encoded, encodeError := base64::encode("foobar");
decoded, decodeError := base64::decode("Zm9vYmFy");
```

`encode_bytes` accepts arbitrary borrowed bytes when the source is not already
a `stringv`. `encode_byte_to_byte` returns newly allocated Base64 bytes from a
borrowed byte slice, while `encode_to_bytes` provides the same output from a
`stringv`. On the decoding side, `decode_from_bytes` accepts borrowed Base64
bytes and returns a byte-oriented `string`. `decode_byte_to_byte` returns newly
allocated binary bytes from a byte slice, while `decode_to_bytes` accepts a
`stringv`. Empty input succeeds and returns an empty result of the requested
type.

Encoding is deterministic and always emits `=` padding when the final group is
incomplete. Decoding is strict: the source length must be a multiple of four,
only the standard `A-Z`, `a-z`, `0-9`, `+`, and `/` alphabet is accepted,
padding may appear only at the end, and discarded bits in the final quartet
must be zero. Whitespace, URL-safe digits, missing padding, extra padding, and
other non-canonical encodings report `Base64Error.InvalidEncoding`.

`encoded_length` reports `Base64Error.Overflow` before allocation when the
encoded size cannot fit in `usize`. Decoding validates the complete input
before allocating its output. Allocation failures from the owning APIs are
returned through their general `error` result.

## Base64url

The `base64url` module uses the RFC 4648 URL- and filename-safe alphabet. Its
padded and unpadded operations are deliberately separate, so callers choose a
single canonical spelling:

```flecha
@imports :: lib::std::encoding::base64url;

padded, paddedError := base64url::encode_padded("token");
unpadded, unpaddedError := base64url::encode_unpadded("token");

decodedPadded, decodePaddedError :=
    base64url::decode_padded(padded.view());
decodedUnpadded, decodeUnpaddedError :=
    base64url::decode_unpadded(unpadded.view());
```

The alphabet uses `-` and `_` instead of `+` and `/`. Padded decoding requires
a multiple-of-four length and canonical trailing `=` bytes. Unpadded decoding
rejects every `=` byte and lengths with a one-character final group. Both forms
reject the standard alphabet, mixed alphabets, whitespace, misplaced or excess
padding, and nonzero discarded bits.

`padded_encoded_length`, `unpadded_encoded_length`,
`padded_decoded_length`, and `unpadded_decoded_length` check sizes and validate
the selected spelling. Matching `_bytes`, `_byte_to_byte`, and `_to_bytes`
operations provide string- and byte-oriented owning results. Decoders validate
the complete source before allocation; allocation failure returns an empty
result with `AllocationError.OutOfMemory`.

## Percent encoding

The `percent` module implements generic RFC 3986 URI-component encoding. It
leaves only `A-Z`, `a-z`, `0-9`, `-`, `.`, `_`, and `~` literal and encodes
every other input byte with an uppercase `%HH` escape:

```flecha
@imports :: lib::std::encoding::percent;

encoded, encodeError := percent::encode("a path/+value");
decoded, decodeError := percent::decode(encoded.view());
```

Input is byte-oriented. UTF-8 receives no special treatment, so each encoded
byte gets its own escape. Decoding accepts either hexadecimal letter case,
requires complete `%HH` triplets, and treats `+` as a literal plus rather than
form-encoded space. It does not parse URLs, query pairs, IDNA, or HTML form
encoding.

`encoded_length` checks expansion overflow and `decoded_length` validates every
escape without allocating. The `_bytes`, `_byte_to_byte`, and `_to_bytes`
operations match the other encoding modules. Decoding validates before
allocation, and allocation failure never exposes partial output.
