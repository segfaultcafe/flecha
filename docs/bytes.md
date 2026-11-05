# Bytes

The exact v0.3 declarations and focused tests are indexed in the
[standard-library reference](standard_library.md#std-bytes-buffer).

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
authenticated := sequence::constant_time_equal(expectedTag, receivedTag);
sequence::zero_bytes(secret);
order := sequence::compare_lexicographic(left, right);
foundAt, found := sequence::index_of(haystack, needle);
```

It provides equality, unsigned lexicographic comparison, prefix and suffix
checks, and byte or subsequence search. These functions neither allocate nor
interpret the bytes as text.

`constant_time_equal` is the security-sensitive equality operation. For inputs
of equal length, its native runtime boundary reads every byte and does not exit
based on byte contents. It returns true for two empty slices. Input lengths are
public: unequal lengths return false before entering the runtime. The operation
does not allocate, block, transfer ownership, or modify either input, and it is
safe for concurrent calls.

Ordinary `equal` may stop at the first differing byte and must not be used to
verify authentication tags or other values whose contents require a
content-independent comparison. Text callers can use
`strings::constant_time_equal(stringv, stringv)`; it compares the complete byte
views without Unicode interpretation or allocation. Constant-time comparison
does not make surrounding validation code or secret-dependent control flow
constant-time.

`zero_bytes` overwrites every byte in a mutable slice with zero through a
non-elidable host operation. It accepts an empty slice, does not allocate or
block, and preserves storage outside the supplied range. The caller remains
responsible for clearing every copy of sensitive material, including unused
capacity and compiler- or algorithm-created temporary values that are not part
of the supplied slice.

Binary serialization and textual binary codecs live under `encoding`; see
[Encoding](encoding.md).
