# Encoding

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

writer := binary::Writer::init(destination);
appendError := writer.writeLittle<u16>(4660);
```

`read_little`, `read_big`, `write_little`, and `write_big` accept the exported
`fixed_unsigned_integer` type set: `u8`, `u16`, `u32`, and `u64`. `usize` is
excluded because its target-dependent width cannot define canonical data.

`read_varint` and `write_varint` use canonical unsigned base-128 encoding.
`varint_length` returns the encoded width. Truncated input, values outside the
target width, and redundant encodings report `BinaryError` without consuming a
cursor or partially modifying a destination.

`Reader` borrows an immutable source and `Writer` borrows a mutable
destination. Their read and write operations are transactional: failure leaves
the cursor position unchanged. Neither cursor allocates.

The Buffer adapters keep serialization ownership in `encoding::binary` rather
than adding format-specific methods to the raw Buffer:

```flecha
binary::append_little<u32>(&buffer, value);
binary::append_big<u64>(&buffer, wide);
binary::append_varint<u32>(&buffer, count);
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
