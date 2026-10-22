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
letter case:

```flecha
@imports :: lib::std::encoding::hex;

written, encodeError := hex::encode(destination, source);
decoded, decodeError := hex::decode(binaryDestination, encodedSource);
```

`encode` emits lowercase characters and `encode_upper` emits uppercase.
Decoding rejects odd lengths and non-hexadecimal characters. All operations
are transactional and write no terminator.

Hex can also target a binary Writer without making the Writer depend on the
codec:

```flecha
writeError := hex::write(&writer, source);
upperError := hex::write_upper(&writer, source);
```

Length overflow, insufficient destination space, and malformed input report
`HexError`.
