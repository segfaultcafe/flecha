# Bytes

The `endian` module reads and writes fixed-width unsigned integers in an
explicit byte order:

```flecha
@imports
    :: lib::std::bytes::endian
    :: lib::std::bytes::errors as byte_errors;

value, readError := endian::read_little<u32>(source);
writeError := endian::write_big<u64>(destination, value64);
```

`read_little` and `read_big` consume the first `bits::width<T>() / 8` bytes of
their source. They return `ByteError.UnexpectedEnd` and a zero value when the
source is too short.

`write_little` and `write_big` modify the first `bits::width<T>() / 8` bytes of
their destination. They return `ByteError.InsufficientSpace` without modifying
the destination when it is too short. All four operations ignore extra bytes.

The operations accept the exported `fixed_unsigned_integer` type set: `u8`,
`u16`, `u32`, and `u64`. `usize` is intentionally excluded because its width
depends on the compilation target and is unsuitable for canonical serialized
data.

## Reader

The `cursor` module provides `Reader`, a borrowed cursor over an immutable byte
slice:

```flecha
@imports
    :: lib::std::bytes::cursor;

[6]u8 encoded = [52, 18, 18, 52, 86, 120];
reader := cursor::Reader::init(encoded[:]);

little, littleError := reader.readLittle<u16>();
big, bigError := reader.readBig<u32>();
```

`Reader` stores no allocation and does not own or copy its source. Its
constructor's borrow-result contract requires the source to outlive the reader.
`position()` reports the consumed byte count, `len()` reports the original
source length, and `remaining()` reports the unread byte count.

`readByte()` consumes one byte. `read(count)` consumes and returns a read-only
view of the next `count` bytes; the returned view borrows the reader and must be
released before advancing it again. A zero-length read succeeds. `readLittle<T>`
and `readBig<T>` decode the same fixed-width unsigned types supported by the
`endian` module.

Every failed read returns `ByteError.UnexpectedEnd` and leaves the cursor
position unchanged. Failed scalar reads return zero. A failed slice read returns
a valid empty view at the current position rather than a null or owning value.
