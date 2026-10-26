# Collection ownership contracts

This document defines the Stage 2 ownership contract for Flecha's growable
collections. It is both a design and acceptance contract. Implementation lands
incrementally; an operation is supported only after its required storage path
and acceptance tests are present.

The first Stage 2 deliverable is an ownership-capable `Array<T>`. `Deque<T>`
is the second and reuses the same storage and lifecycle rules. Stacks, queues,
binary heaps, and hash maps now build on those foundations; hash sets follow.

**Acceptance status:** complete. The initialized-prefix implementation and its
safe API now pass the full matrix below for allocation failure, move-only
lifecycle, direct owning strings and slices, recursive nested ownership, and
shared/exclusive view conflicts. The deque, stack, queue, binary-heap, and
hash-map gates described below are also complete, so Stage 2 can proceed to
hash sets.

## Supported element categories

`Array<T>` supports:

- ordinary copyable values, including values with nontrivial semantic copies;
- whole-value movable structures with safe teardown;
- owning strings, owning slices, and other movable owning values; and
- nested owning collections such as `Array<Array<T>>`.

A structure with an unsafe `tear` is immovable and cannot be an array element.
Borrowed references and slices are not stored in this first pass; collection
storage needs a separate lifetime contract before borrowed elements can be
accepted.

Copyability is not required merely to store, grow, remove, or destroy an
element. Operations whose meaning inherently creates a second value remain
copy-only and are rejected when specialized for a move-only `T`.

## Copy and move behavior

Flecha keeps one by-value API rather than separate copy and move overloads.
Passing a copyable value makes the language's normal semantic copy. Passing a
noncopyable but movable value transfers it and invalidates the caller's source.
The callee owns that parameter even when the operation later returns an error.

The first pass retains the existing public shapes:

```flecha
error Array<T>::push(&self, T value);
error Array<T>::set(&self, usize index, T value);
(T, error) Array<T>::pop(&self);
(T, error) Array<T>::remove(&self, usize index);
(T, error) Array<T>::swapRemove(&self, usize index);
void Array<T>::clear(&self);
error Array<T>::reserve(&self, usize capacity);
```

The value component of a failed extraction is the contextual `none` value,
following the existing Flecha `(value, error)` convention. Element types with
custom initialization or teardown already require an explicit `zero()` and
therefore retain a valid `none` representation.

The Stage 2 `Array<T>` operations have these contracts:

| Operation | Contract |
| --- | --- |
| `push(T value)` | Appends `value`. A copyable argument is copied; a movable argument is consumed. Growth failure leaves the array unchanged, but the already-passed argument remains consumed and is destroyed exactly once by the failed call. |
| `set(usize index, T value)` | Replaces one live element. The replacement is evaluated and transferred before the old element is destroyed. An invalid index leaves the array unchanged; the passed value is still owned and destroyed by the call. |
| `pop()` | Removes and returns the final element by transferring it out of storage without creating a semantic duplicate. Empty arrays return `ArrayError.Empty` and the contextual `none` result. |
| `remove(usize index)` | Removes and returns the indexed element, then relocates later elements left while preserving order. An invalid index returns `ArrayError.IndexOutOfBounds` without mutation. |
| `swapRemove(usize index)` | Removes and returns the indexed element. Unless it was last, the final element is relocated into the gap. An invalid index returns `ArrayError.IndexOutOfBounds` without mutation. |
| `clear()` | Destroys every live element in reverse index order, sets the length to zero, and retains the allocation. |
| `reserve(usize capacity)` | Ensures at least `capacity` slots without changing length or logical values. Allocation failure or capacity overflow leaves storage, length, capacity, and every element unchanged. |

`pop`, `remove`, and `swapRemove` are ownership-transferring extractions even
when `T` is copyable. They do not allocate after validation and do not leave a
second live value in the vacated slot.

The following existing operations remain copy-only because they deliberately
produce independent values: `fromSlice`, `append` from a borrowed slice,
`clone`, `get`, `first`, `last`, and `fill`. Specializing the first six for a
move-only element reports `FLECHA-S0046`; `fill` reports `FLECHA-S0059` because
its first assignment consumes the value and another loop iteration may reuse
it. Move-only elements are inspected through `view()` or `viewRef()` and are
extracted only through the ownership-transferring removal operations.

## Initialized-prefix storage

An ownership-capable collection cannot use a fully initialized
`new<[]T>(capacity)` buffer. Capacity is storage, not a number of live `T`
values. The internal representation obeys these invariants:

1. `0 <= length <= capacity` always holds.
2. Slots in `[0, length)` each contain exactly one live `T`.
3. Slots in `[length, capacity)` are uninitialized storage. They are never
   read, borrowed, or destroyed.
4. Public views expose exactly `[0, length)`, never spare capacity.
5. Each live slot has one cleanup responsibility. Relocation transfers that
   responsibility and immediately makes the source slot non-live.
6. Destroying an array destroys its live elements in reverse index order and
   then releases the backing allocation.

The compiler/runtime must provide a narrowly scoped internal facility capable
of:

- allocating aligned storage for `capacity` values without constructing them;
- initializing an uninitialized slot from a complete value;
- relocating a live slot into an uninitialized slot without copying it;
- extracting a live slot as a complete value and marking the slot uninitialized;
- destroying a live slot exactly once; and
- releasing storage without implicitly destroying uninitialized capacity.

The initial implementation exposes these operations to the standard library as
reserved `__storage_*` compiler intrinsics. Every intrinsic requires an
`unsafe` function or block at its call site; safe application code cannot
allocate, initialize, relocate, destroy, or release raw collection storage.

The syntax and compiler boundary for this facility are an implementation
decision. It must not expose general uninitialized memory to safe application
code as an accidental side effect of adding collections.

Growth is a two-phase operation. It first allocates replacement storage. If
allocation fails, nothing has moved and the original array is untouched. After
allocation succeeds, relocation of the initialized prefix is infallible: each
element moves once into the replacement, the old slots become non-live, and
the array commits the replacement storage and capacity. It then releases the
old allocation without destroying those moved values.

## Nested owning collections

`Array<Array<T>>` is the acceptance case for recursive ownership. Pushing an
inner array transfers the complete inner owner into one outer slot. Growing or
compacting the outer array relocates that owner; it does not clone the inner
buffer or its elements. Removing an inner array returns the same logical owner,
and destroying the outer array destroys every remaining inner array exactly
once.

The same rules apply recursively to deeper nesting and to owning element types
such as `own []T`. An active borrow from an inner collection protects both the
inner owner and every enclosing owner whose movement would relocate it.

## Borrowed views

`view()` returns a shared `[]const T` borrowed from the complete array owner.
While that view is live:

- additional shared reads and shared views are allowed;
- element mutation and every mutable array method are rejected; and
- the array cannot be moved, replaced, destroyed, cleared, or reallocated.

`viewRef()` returns an exclusive `[]T` borrowed from the complete array owner.
While that view is live, access through the array binding is suspended. The
view may mutate existing elements according to ordinary slice rules, but it
cannot change the array's length or capacity. No other shared or mutable view
may overlap it.

The initial policy deliberately treats the whole array as one borrow region.
It does not prove disjoint element or range borrows. A borrow ends at its last
possible use under the existing control-flow-aware borrow checker. Even a
`reserve` call that would be a runtime no-op is rejected while a conflicting
view is live because method validity cannot depend on the current capacity.

## Failure and cleanup guarantees

- Bounds errors never mutate the array.
- Allocation failure and capacity overflow preserve the array's prior length,
  capacity, elements, and element order.
- A by-value argument belongs to the called operation before its body runs;
  failure does not restore a moved caller variable.
- Replacement evaluates and acquires the new value before destroying the old
  value.
- Removal transfers the removed value and destroys no duplicate of it.
- Compaction and growth relocate elements; they do not semantically copy them.
- All normal exits, including early returns and loop exits inside the
  implementation, leave every slot either live with one cleanup obligation or
  uninitialized with none.
- Process-terminating runtime safety failures need not unwind, consistently
  with the language memory model.

## Definition of done

The ownership-capable array foundation satisfies all of the following gates:

- the initialized-prefix storage facility is implemented and unavailable to
  ordinary safe application code;
- every operation above follows its copy, move, error, and cleanup contract;
- copy-only APIs diagnose move-only specializations at compile time;
- immovable element types are rejected before object emission;
- `Array<Array<T>>` survives outer and inner growth, removal, movement, and
  destruction without cloning an inner allocation;
- allocation-failure injection proves the strong array guarantee at initial
  growth and later reallocation;
- lifecycle tests cover copyable scalars, nontrivially copyable values,
  movable destructible values, owning slices, and nested arrays;
- cleanup instrumentation and sanitizer runs prove no leak, double cleanup,
  use after move, read from an uninitialized slot, or destruction of spare
  capacity;
- compile-fail tests cover use after move, partial indexed moves, immovable
  elements, copy-only APIs on move-only elements, and every conflicting-view
  case; and
- existing array, slice, byte-buffer, ownership, and compiler diagnostic tests
  remain green.

## Required test matrix

| Area | Required cases |
| --- | --- |
| Push and replacement | Copyable lvalue, movable lvalue, temporary value, spare-capacity append, growing append, valid replacement, invalid replacement, and argument cleanup on error. |
| Extraction | Singleton and multi-element `pop`; first, middle, and last `remove`; first, middle, and last `swapRemove`; empty and invalid cases; returned owner remains usable. |
| Relocation | Growth from zero, multiple growth steps, exact-capacity reserve, reserve below current capacity, ordered compaction, unordered compaction, and preservation of element resource identity. |
| Failure | Initial allocation failure, reallocation failure, capacity overflow, unchanged length/capacity/order, and exactly-once cleanup of a consumed push argument. |
| Cleanup | Clear empty and populated arrays, capacity reuse after clear, destruction without clear, replacement cleanup, reverse destruction order, early exits, and no cleanup of spare capacity. |
| Element categories | Every scalar family, owning string, copyable structure with owned fields, movable safe-tear structure, owning slice, nested array, and compile-time rejection of unsafe-tear immovable structures. |
| Nested ownership | Empty inner arrays, populated inner arrays, outer growth, inner growth before and after insertion, removal from the outer array, three-level nesting, and destruction at each level. |
| Borrowing | Multiple shared views; shared-versus-mutable conflicts; exclusive-view conflicts; move, replacement, clear, reserve, push, and destruction while borrowed; access allowed after last use. |
| Copy-only surface | Rejection of `fromSlice`, borrowed-slice `append`, `clone`, `get`, `first`, `last`, and `fill` for a move-only `T`, without masking positive move-aware tests. |

## Deque acceptance

`Deque<T>` owns circular `RawStorage<T>`. Its live elements are the logical
range `[0, len)`, beginning at `head`; every other slot is uninitialized spare
capacity. Growth relocates the live range into logical order in a replacement
allocation and commits only after allocation succeeds.

The safe API provides checked indexed `get` and `set`, endpoint access through
`front` and `back`, insertion and ownership-transferring extraction at both
ends, capacity reservation, clearing, and length/capacity queries. `get`,
`front`, and `back` remain copy-only. Mutation, growth, extraction, clearing,
and destruction support movable noncopyable elements.

The deque gate covers:

- initial allocation failure and later growth failure from both ends, with the
  consumed argument destroyed once and all existing state preserved;
- circular growth, linearization, reverse logical destruction, `clear`, spare
  capacity reuse, and early-return cleanup;
- owning strings and slices plus two- and three-level nested deques;
- rejection of immovable and borrowed elements, use after movement, private
  storage access, and copy-only endpoint operations on move-only elements; and
- allocation auditing plus focused ASan/UBSan execution.

`Deque<T>` is complete for the Stage 2 foundation. `Stack<T>` and `Queue<T>`
are complete thin safe adapters over the array and deque foundations.

## Binary heap acceptance

`BinaryHeap<T>` is a max-oriented heap backed directly by `RawStorage<T>`.
Callers provide a `Less<T>` policy whose result is true when its left argument
has lower priority than its right argument. Reversing that policy produces a
min-oriented heap, and capturing callables are retained as part of the heap.

The safe API provides `push`, ownership-transferring `pop`, copy-only `top`,
capacity reservation, clearing, and length/capacity queries. Sifting exchanges
live slots with move-only `RawStorage.swap`; it never creates semantic copies
of elements. Allocation failure occurs before any relocation, preserving the
existing heap and destroying a consumed rejected argument exactly once.

The binary-heap gate covers:

- scalar max- and min-oriented ordering plus capturing comparison policies;
- initial and growth allocation failure with the existing heap unchanged;
- move-only sifting, growth, extraction, clearing, capacity reuse, destruction,
  and early-return cleanup;
- two- and three-level nested heaps without cloning inner allocations;
- rejection of immovable and borrowed elements, use after movement, private
  representation access, and copy-only `top` on move-only elements; and
- allocation auditing plus focused ASan/UBSan execution.

`BinaryHeap<T>` is complete for the Stage 2 foundation. The hash-map foundation
is complete below; hash set follows it.

## Hash map acceptance

`HashMap<K, V>` is an open-addressed table with replaceable `Hash<K>` and
`Equal<K>` policies. Its initial safe API provides insertion and
replacement through `set`, copy-only lookup through `get`, noncopying
`containsKey`, ownership-transferring `remove`, capacity reservation, clearing,
length/capacity queries, and noncopying traversal through `forEach`. Traversal
borrows each key and value and does not promise bucket order.

Keys and values occupy parallel `RawStorage` allocations. A separate byte-state
array distinguishes empty, occupied, and deleted buckets, so neither keys nor
values need sentinel representations and spare buckets are never initialized or
destroyed as `K` or `V`. Linear probing preserves collision chains across
deletions, growth rehashes only occupied buckets, and allocation completes
before any existing entry is relocated.

The initializer uses generic default policies. Its correctness-first fallback
hash places every key in one collision chain; callers that need normal
open-addressing performance replace it while building the empty map. The
consuming `self` receiver permits safe chaining on temporaries, and the
multi-origin result contract retains borrows carried by both the map and the
new policy:

```flecha
map := HashMap<K, V>::init()
    .withHash(hash)
    .withEqual(equal);
```

Policy replacement on a nonempty map is ignored so existing bucket placement
and equality semantics remain valid.

The hash-map gate covers:

- empty and missing-key behavior, insertion, replacement, growth, forced
  collisions, deleted-bucket reuse, removal, clear/reuse, and iteration;
- move-only keys and values across replacement, relocation, extraction,
  traversal, clearing, and destruction with exactly-once cleanup;
- failure injection at each of the state, key-storage, and value-storage
  allocations during both initial insertion and later growth, preserving all
  existing entries while consuming rejected arguments exactly once;
- two- and three-level nested maps through outer and inner growth, extraction,
  clearing, and destruction;
- rejection of immovable and borrowed keys or values, use after movement,
  private representation access, mutation during an active borrow, and
  copy-only `get` on move-only values; and
- allocation auditing plus dedicated ASan/UBSan and CI execution.

`HashMap<K, V>` is complete for the Stage 2 foundation. Hash set reuse is the
next hash-based collection layer.
