# Bytes

The `bytes` namespace owns raw byte storage and sequence operations. It does
not own serialization formats, textual codecs, or structured data formats.

## Buffer

The `buffer` module provides a dynamically growing owning byte sequence:

```flecha
@imports :: lib::std::bytes::buffer;

value, allocationError := buffer::Buffer::withCapacity(16);
if allocationError != none {
    return allocationError;
}

value.writeByte(42);
value.write(source);
bytes := value.bytes();
```

`Buffer` tracks initialized length separately from allocation capacity.
`bytes()` exposes only the initialized prefix. `reserve`, `write`, `writeByte`,
and growing `resize` may allocate and report an allocation error without
discarding existing contents. `clear`, shrinking `resize`, and `truncate`
retain the allocation.

`popByte` removes the final byte and reports `BufferError.Empty` without
changing an empty Buffer. `writeString` appends the existing bytes of a
`stringv`; it does not validate, normalize, or otherwise interpret text.

## Sequence

The `sequence` module operates on borrowed byte slices:

```flecha
@imports :: lib::std::bytes::sequence;

same := sequence::equal(left, right);
order := sequence::compare_lexicographic(left, right);
foundAt, found := sequence::index_of(haystack, needle);
```

It provides equality, unsigned lexicographic comparison, prefix and suffix
checks, and byte or subsequence search. These functions neither allocate nor
interpret the bytes as text.

Binary serialization and textual binary codecs live under `encoding`; see
[Encoding](encoding.md).
