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
