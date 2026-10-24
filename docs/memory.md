# Memory

The `memory` namespace owns safe operations on typed borrowed slices. These
operations copy, replace, and destroy elements according to the ordinary
language rules for `T`; they never treat a typed slice as unstructured bytes.
Raw byte equality and lexicographic ordering belong to
[Bytes](bytes.md), while serialization belongs to [Encoding](encoding.md).

```flecha
@imports
    :: lib::std::memory::slices
    :: lib::std::memory::errors as mem_errors;
```

## Element requirements

The Stage 1 slice operations support any `T` that ordinary Flecha assignment
can copy. This includes copyable structures and structures whose owned fields
require nontrivial copy or cleanup. Each copied element is an independent
semantic copy, and replacing an initialized destination performs the normal
cleanup for its previous value exactly once.

Flecha does not yet have a general behavioral constraint for copyability, so
the requirement cannot appear in the generic parameter list. Specializing one
of these functions with a move-only or immovable element type is rejected at
compile time by the normal lifecycle diagnostics. Copying from an element
reports a prohibited partial copy (`FLECHA-S0046`); repeatedly using a
move-only fill value reports move reuse (`FLECHA-S0059`). The functions do not
silently move from a source, duplicate ownership bits, or fall back to a raw
byte copy. Move-aware slice algorithms are outside the Stage 1 contract.

## Copying between slices

```flecha
count := slices::copy_elements<T>(destination, source);
```

`copy_elements` copies `min(destination.len(), source.len())` elements in index
order and returns that count. A shorter destination intentionally truncates
the copy; a longer destination retains its untouched suffix. An empty source
or destination returns zero and changes nothing.

The source and destination must satisfy Flecha's ordinary simultaneous borrow
rules. In particular, this API does not promise support for overlapping views
of the same storage. Use `copy_within` when the source and destination are
ranges in one slice.

## Copying within one slice

```flecha
copyError := slices::copy_within<T>(
    values,
    sourceStart,
    sourceEnd,
    destinationStart,
);
```

The source is the half-open range `[sourceStart, sourceEnd)`. Its complete
contents are copied to the equally sized range beginning at `destinationStart`.
The operation behaves as though the original source range were preserved even
when the two ranges overlap: a rightward overlap copies from the end toward the
front, and every other case copies from the front toward the end. No temporary
allocation is required.

Validation is completed before the first element is changed. Checks occur in
the following order, and the first applicable error is returned:

- `sourceStart > sourceEnd` returns `SliceError.InvalidRange`;
- `sourceStart` or `sourceEnd` beyond `values.len()` returns
  `SliceError.IndexOutOfBounds`;
- `destinationStart` beyond `values.len()`, or a destination range that would
  extend past the end, returns `SliceError.IndexOutOfBounds`; and
- a valid empty source range succeeds, including a range and destination that
  both begin at `values.len()`.

Invalid input leaves the slice unchanged. A valid copy to the same starting
index is a no-op. Success returns `none`.

## Filling

```flecha
slices::fill<T>(values, value);
```

`fill` replaces every element, in index order, with a semantic copy of `value`.
An empty slice is a no-op. For a nontrivially copyable `T`, each destination
receives its own copy and its previous value is cleaned up normally. The
operation follows ordinary copy-assignment failure behavior and adds no
transactional rollback guarantee.

## Swapping

```flecha
swapError := slices::swap<T>(values, left, right);
```

`swap` validates both indices before changing either element. If either index
is outside the slice, it returns `SliceError.IndexOutOfBounds` and leaves the
slice unchanged. Equal valid indices succeed as a no-op. Distinct valid
indices exchange the two values using their ordinary semantic copy behavior.
Success returns `none`.

## Slice errors

`errors::SliceError` contains exactly these Stage 1 variants:

| Variant | Meaning |
| --- | --- |
| `InvalidRange` | A half-open range has its start after its end. |
| `IndexOutOfBounds` | An index or either endpoint of the resulting operation lies outside the slice. |

The variants intentionally do not carry indices or lengths. More detailed
diagnostics can be added later without conflating malformed range ordering
with a range that is well ordered but outside its storage.
