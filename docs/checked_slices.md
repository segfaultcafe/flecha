# Checked slices

A slice borrows contiguous initialized storage. Its runtime representation is an
address and a `usize` length, with no capacity, allocation, ownership, or cleanup.
The source array owns the elements. See the runnable
[example](../examples/checked_slices.fl).

## Types and creation

```flecha
[4]i32 values = [10, 20, 30, 40];
[]i32 middle = values[1:3];
whole := values[:];
tail := values[1:];
head := values[:2];
```

These are alternative borrows: simultaneous conflicting uses of mutable views
are rejected. A range is half-open, so `values[1:3]` exposes indices 1 and 2.
An omitted lower bound is zero; an omitted upper bound is the receiver's length.
Subslice with the same syntax, such as `middle[1:]`. Bounds must be `usize`;
integer literals acquire that context, but other typed integers need casts.

`[]T` permits element mutation. `[]const T` permits only reads. `const []T` fixes
the slice handle while permitting mutation of its elements. A const array
produces a read-only slice, and an expected `[]const T` type creates a shared
borrow from writable storage:

```flecha
[]const i32 shared = values[:];
```

Supported elements match fixed-array value elements: scalars, error values, raw
pointers, structures, and nested fixed arrays. Structure elements follow the
copy and borrow restrictions in [structures.md](structures.md). Element-level const is allowed on slices. Slices require
initializers; `none` and `null` are not valid slice values. Empty views are made
with `values[i:i]`, including `i == values.len()`, or by slicing a zero-length
array. Empty views still carry the source lifetime and borrow restrictions.

Slicing an array temporary or a raw array pointee is rejected. A checked array
reference can be used explicitly: `(*reference)[:]`. There is no implicit
array-to-slice conversion; pass `values[:]`. Nested reference/slice storage,
arrays of slices, pointers/references to slice handles, slice casts/comparisons,
and slice returns are rejected. Returning borrowed results needs a future
lifetime contract.

## Accessors

| Expression | Behavior |
| --- | --- |
| `slice.len()` | Returns its `usize` length |
| `slice[index]` | Checked element location |
| `slice.front()` | Checked first element location |
| `slice.back()` | Checked last element location |
| `slice.data()` | Unsafe raw pointer to the first element position |

`front()` and `back()` also work on fixed arrays. They take no arguments, support
writes for mutable elements, and can be borrowed with `&`, just like indexing:

```flecha
middle.front() = 42;
middle.back()++;
ref := &middle[0];
*ref = 7;
```

Reading an element copies its value; these methods do not implicitly return a
reference handle. Empty endpoint access traps, or is rejected statically for
known-empty fixed arrays. `.len()` is safe on an initialized empty slice.

`slice.data()` is the explicit unsafe bridge for synchronous native APIs. It
returns `*T` for `[]T` and `*const T` for `[]const T`, requires an unsafe block,
and does not check that the slice is non-empty. For an empty slice the pointer
is only a position and must not be dereferenced. The pointer carries no checked
lifetime or alias tracking; unsafe code is responsible for keeping the backing
owner alive, respecting element constness, and preventing invalid aliasing.

## Bounds and evaluation

Slicing checks `0 <= start <= end <= length`. Indexing checks `index < length`.
Unsigned bounds make negative values invalid, including values cast from signed
integers. Known-invalid array slice bounds or known reversed endpoints are
compile errors (`FLECHA-S0037`). Other failures trap before pointer arithmetic or
element access, including in unsafe blocks. Traps do not unwind cleanup scopes.

The receiver evaluates once, followed by the lower bound and upper bound, each
once. Omitted bounds use the captured receiver. Index/member locations capture
the receiver once and check bounds before a store's right-hand side runs.

## Borrowing and calls

Mutable handles move on assignment; shared read-only handles copy. A moved
handle cannot be used until it is rebound. Fixed mutable handles cannot move;
use a subslice to reborrow instead. Assignment rebinds the handle and preserves
its declared type. To create a read-only reborrow from a mutable slice, use
`[]const T shared = mutable[:]`.

Function arguments reborrow for the call, preserving the caller's handle:

```flecha
void increment([]i32 values) {
    loop usize i = 0; i < values.len(); i++ { values[i]++; }
}
i32 first([]const i32 values) { return values.front(); }
```

The checker treats the entire backing array as one borrow region. Distinct
ranges do not yet prove disjointness. Mutable slices exclude conflicting owner
access; shared slices exclude mutation. Subslices and element references retain
their parent borrow and prohibit conflicting parent access until their last
possible use. A handle involved in a pending index/store/bounds evaluation cannot
be moved or rebound during that evaluation. Scope checks prevent slices from
outliving either their owner or a parent reborrow. These rules also apply through
branches, loops, positions, and jumps. General raw-address conversion of slice
handles or slice-derived element locations remains rejected; unsafe `.data()`
is the sole direct storage-pointer escape hatch.

## Iteration

```flecha
for index := range slice {}
for (index, value) := range slice {}
for (usize index, i32 value) = range slice {}
```

A slice range evaluates its source once and takes a shared reborrow for the loop.
The descriptor is copied, not the elements. The borrow prevents changes to the
backing storage during iteration. Each element binding is a value copy; the
original handle remains usable after the loop. Indices use `usize`, including
explicit declarations, because a slice's length is dynamic. Counted loops can
mutate elements through the same slice. Fixed-array ranges retain their existing
array snapshot semantics.

## Scope and verification

A slice may be returned when its function declares the checked-reference or
checked-slice parameter that anchors it, for example
`[]const i32 view([]const i32 values) <- values`. The caller retains a loan on
the actual source for as long as the returned slice is live. See
[reference and slice returns](ref_and_slice_returns.md) for contracts,
multi-results, and conservative boundaries.

Slices have no `push()`, `pop_back()`, `iter()`, or `stitch()` method. Growth and explicit
iterator handles are reserved for future container types.

The native calling convention for slice descriptors is internal to Flecha;
this does not define a C ABI. Tests cover parser recovery, type/borrow/flow
errors, LLVM verification, calls, empty/nested views, element references,
read-only range reborrowing, evaluation order, and bounds traps. Sanitizer runs
exercise the compiler and instrumented native harness/runtime.
