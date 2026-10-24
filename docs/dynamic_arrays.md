# Runtime arrays and the first standard-library container

The safe owning form is `own []T`. Allocate it with
`new<own []T>(count)`, which returns `(own []T, error)`. The owner carries its
data pointer and `usize` length, is move-only, and releases the allocation
automatically when its live value leaves scope. It can be stored directly in a
structure and does not require `unsafe`, `defer`, or `free`:

```flecha
structure Buffer {
    own []u8 bytes;
};

Buffer Buffer::init(own []u8 bytes) override {
    return build Buffer { self.bytes = bytes; };
}

void example() {
    bytes, err := new<own []u8>(100);
    if err != none { return; }
    bytes[42] = 42;
    view := bytes[:];
}
```

Indexing, slicing, `front`, `back`, and `len` are checked safe operations on an
owning slice. `data()` remains unsafe because it exposes a raw pointer. Adding
`const` to the element type is a one-way permission weakening:
`own []T` can move into `own []const T`, but not the reverse.

An empty owning slice is trivially zeroable. Structures containing owning
slices therefore receive a generated trivial `zero()` when their other fields
are also trivially zeroable. An equivalent explicit `zero()` override remains
trivial, so `none` can directly construct those structures, including within
tuple results. The structure remains move-only because zero construction does
not duplicate a live owner.

The original manual form remains available for raw-memory work and backwards
compatibility:

Runtime-sized storage uses `new<[]T>(count)` in an unsafe context. `count` has type
`usize` and is evaluated exactly once, before attempting allocation. The result is
`(*[]T, error)`: `*[]T` is a move-only owning buffer containing the allocation
base and its `usize` element count. Numeric, boolean,
character, and defaultable enum elements are supported. Runtime arrays also
accept copyable, default-constructible structures whose recursively stored
fields are numbers, booleans, characters, enums, raw pointers, fixed arrays, or
other supported structures. Every slot is default-constructed after allocation
succeeds; custom `zero()` overrides therefore run once per slot. Strings, pointer
elements, destructible/noncopyable structures, and partially initialized
storage are not supported by this runtime-array form.

```flecha
unsafe {
    buffer, err := new<[]i32>(100);
    if err != none {
        return 1;
    }
    defer free(buffer);
    buffer[42] = 42;
}
```

Multiplication overflow or allocation failure returns `{null, 0}` and
`AllocationError.OutOfMemory`. A zero count can succeed with a non-null,
releasable handle whose length is zero; no element may be accessed. `const`
controls mutation as before: `new<[]const i32>(count)` produces `*[]const i32`.

Owning-buffer indexing, slicing, `front`, `back`, and `len` use the count carried
in the value and require unsafe permission. Indexing is bounds checked before an
element address is formed. Slicing produces an ordinary borrowed `[]T` descriptor.
Moves, consuming arguments and returns, and `take` invalidate the source owner;
`free(buffer)` consumes the live allocation obligation.

The null owning-buffer value is trivially zeroable. A structure with an
equivalent explicit `zero()` override can therefore use `none` directly,
including when nested in a tuple result. The structure remains move-only:
zero construction creates no live owner and never duplicates one.

Legacy raw `*T` indexing still requires unsafe permission and a base pointer from
Flecha's allocator. The index has type `usize` and is checked against allocator
metadata before forming an element address. Out-of-bounds access traps. Known stack and
interior addresses, known null pointers, and tracked freed/transferred aliases are
rejected. Arbitrary external pointer provenance remains an unsafe caller obligation:
indexing a foreign allocation or interior address cannot read Flecha's metadata.
For source compatibility, an owning buffer may transfer into an explicitly typed
raw `*T` destination; this consumes the owner and drops its visible length. New
code should retain `*[]T` so checked access does not consult hidden metadata.
This does not add pointer arithmetic or checked references into the heap. Ordinary
raw dereference `*pointer` retains its unsafe validity
obligations, including requiring a live element for a zero-capacity allocation.

`free(pointer)` releases the complete buffer. The supported element types need no
individual destruction. Ordinary `new<T>(value)` still allocates one value and
runs typed destruction, including `tear`; its initializer still runs only after
allocation succeeds. Runtime-array counts, unlike initializers, must run even
when allocation subsequently fails.

## Replacing an owning field

`take(self->field)` transfers an owning raw pointer out of a mutable field and
sets that field to null. It is unsafe and currently limited to owning fields of
the current method receiver. It is a contextual intrinsic: a user-declared
`take` in scope takes precedence. The extracted pointer must be freed or transferred.
Taking a null field returns null. `defer take(...)` is rejected because it would
discard the extracted ownership.

A method may assign a new owner only when the field is known null, including after
`take`. Replacing a live owner is rejected. Aliases from before extraction or
transfer become unavailable. Pending deferred uses prevent conflicting extraction.
Mutable calls accepting a reference to the owning structure conservatively
invalidate tracked aliases to its owning fields; aliases must be obtained again
after the call. General field-effect summaries are not yet implemented.

Construction and destruction use the ordinary lifecycle rules: `tear` is
compiler-invoked, a safe tear makes a structure noncopyable but movable as a
whole value, and an unsafe tear makes it immovable.

## Standard-library `RawArray<T>`

The implementation is Flecha source in `std/src/containers/raw_array.fl`. A project
can declare a local `std` dependency in `longbow.toml`, then import:

```flecha
@imports :: lib::std::containers::raw_array;
```

Construct `raw_array::RawArray<i32>::init()` in a safe context, substituting
any supported scalar or structure type for `i32`. Structure elements must meet
the runtime-array restrictions above. Pointer, string, reference, owning, and
destructible elements are not supported.

The name reflects its unsafe API and raw allocated storage. It is not a safe
dynamic-array abstraction. It maintains a used length and an allocated capacity,
starts empty, grows from four slots by doubling, and releases its buffer through
`tear`.

| Method | Result and behavior |
| --- | --- |
| `len()` | Used element count, `usize` |
| `capacity()` | Allocated element count, `usize` |
| `reserve(capacity)` | `error`; increases capacity if needed |
| `push(value)` | `error`; appends, growing if necessary |
| `pop()` | `(T, error)`; removes the last value or returns `ArrayError.Empty` |
| `get(index)` | `(T, error)`; returns a value or `ArrayError.IndexOutOfBounds` |
| `set(index, value)` | `error`; replaces an existing element or reports bounds |
| `clear()` | Empties the used prefix while retaining capacity |

Allocation, growth, element-access, and removal methods are unsafe in this initial
container. `len`, `capacity`, and `clear` use checked receiver references. Elements
are returned by value; the API exposes no raw buffer pointer or borrowed element
reference. Bounds errors leave the array unchanged. Growth allocates and copies
before extracting the old buffer, so allocation failure preserves length,
capacity, and contents. Overflow is reported as `AllocationError.OutOfMemory`.

See `examples/dynamic_array` for a Longbow project that grows to 100 integers,
prints element 42, and exits with status 42. `StandardLibraryTests.RawArray`
checks 1,000 elements, injected growth failure, overflow, bounds, mutation,
clearing, independent arrays, and exact cleanup of all buffers.

`StandardLibraryTests.RawArrayScalarTypes` exercises all 14 supported scalar
types, including empty results, growth, bounds, mutation, and capacity reuse.
`StandardLibraryTests.RawArrayStructures` exercises the same generic API with a
custom-defaulted `Token` structure, including growth, empty/error defaults,
field-wise equality, and cleanup.

## Safe `Array<T>`

`std/src/containers/array.fl` wraps a private `RawArray<T>` and provides the same
methods through a safe public interface. Construction and all method calls work
outside unsafe blocks. Raw storage operations remain inside the implementation.
Both containers use the shared `errors::ArrayError` from
`std/src/containers/errors.fl`; `AllocationError` remains built-in.

```flecha
@imports
    :: lib::std::containers::array
    :: lib::std::containers::errors;
```

Construct with `array::Array<i32>::init()`. The collection returns elements by
value without exposing backing pointers or uninitialized capacity. Bounds
errors check the used length. Growth failure preserves length, capacity, and
existing contents.

`RawArray` now has a safe initializer and a safe `tear` containing internal
unsafe cleanup. The wrapper's generated lifecycle initializes and destroys its
inner field automatically; it does not free the buffer separately. Both types
remain noncopyable but are movable as complete values. Initialization,
assignment, by-value arguments and returns, and discard transfer the buffer
cleanup responsibility and invalidate the source until it is reassigned. A
live borrow from `view()` or `viewRef()` prevents moving, replacing, or
destroying its container. The language's unsafe-tear immovability rule is
unchanged.

Stage 2's first vertical slices give `push`, `set`, `pop`, growth, `reserve`,
`clear`, and array destruction initialized-prefix storage semantics. `push`
and `set` copy a copyable argument or consume a move-only argument; growth
relocates live elements, and `pop` extracts its result without copying it.
`get`, `remove`, `swapRemove`, and the remaining copy-only surface have not yet
completed the move-only contract. Indexing still cannot partially move an
element out of a container.

`StandardLibraryTests.SafeArray.*` runs growth/failure, scalar, structure, and
scope-cleanup checks through safe callers with allocation tracking.
`CompilerDiagnostics.SafeArray` checks privacy, use after a container move,
conflicting borrows, and unsupported element operations.

The complete Stage 2 extension for movable and destructible elements,
including nested owning arrays, is specified in
[`collections.md`](collections.md). Its definition of done remains the gate
for claiming full move-only `Array<T>` support.
