# Fixed arrays

Fixed arrays are implemented with inline contiguous storage. `[N]T` contains
exactly `N` elements of type `T`; both the element type and length participate
in type identity. Creating, copying, passing, or returning an array does not
allocate a separate heap buffer. See the runnable
[sorting and initialization example](../examples/fixed_arrays.fl).

## Types and initialization

```flecha
[4]u8 values = [10, 20, 30, 40];
[4]u8 zeros = none;
[4]u8 repeated = [7; 4];
inferred := [1, 2, 3]; // [3]i32
[0]u8 empty = [];
[2][3]u8 grid = [[1, 2, 3], [4, 5, 6]];
```

Lengths in types and repeat initializers are nonnegative integer literals.
Named constants and arbitrary expressions in those positions are not enabled.
Array literals allow a trailing comma. List element count must match the
expected length exactly; missing elements are not implicitly zero-filled.
`none` initializes the entire array recursively with defined zero values.

Explicit types provide contextual element types, including integer literal
range checks. Without an expected type, the first initializer element sets the
element type and subsequent elements must be assignable to it. Empty literals
need an expected array type; `[value; 0]` can infer its type from `value`.

List elements evaluate once each, left to right. A repeat initializer evaluates
its value exactly once. Copyable values are copied into every element. A
noncopyable movable value is accepted only for counts zero and one: zero
destroys the evaluated value immediately, while one transfers it into the sole
element. Larger counts would duplicate ownership and are rejected.

Supported elements are integer/floating-point scalars, booleans, characters,
enums, raw pointers, structures, and nested arrays of these types. Reference
handles, strings, views, void, and other non-value element types are rejected,
even at length zero. Structure elements use their normal copy, move,
destruction, and immovability rules.

Native layout arithmetic is checked before lowering. Lengths and storage sizes
must fit the implementation's conservative limit of `PTRDIFF_MAX / 8` bytes
(or elements for a zero-sized element), keeping both byte and bit extents
representable. Nested element types must have valid layouts even inside an
empty outer array. A valid type does not guarantee enough runtime stack space
for an arbitrarily large local value.

Delayed initialization requires whole-array assignment on every incoming path
before reading, indexing, querying `.len()`, or borrowing:

```flecha
[2]u8 values;
values = [1, 2];
values[0] = 3;
```

Writing individual elements does not establish definite initialization yet.

## Copies, parameters, and returns

```flecha
[2]u8 make() { return [1, 2]; }

[2]u8 changed([2]u8 value) {
    value[0] = 42;
    return value;
}
```

An array copies on assignment, passing, and return when every element is
copyable. A copy owns its own inline storage; modifying it does not modify the
source. An array containing a noncopyable movable structure instead moves as a
complete value in those contexts, as well as when discarded or embedded in a
complete tuple transfer. The source becomes unavailable until reassigned, and
cleanup transfers to the destination. Nested arrays apply the rule recursively.
Copying raw pointers copies addresses and neither clones nor frees the pointees.
Array casts, comparisons, implicit decay to a pointer, and implicit conversion
between different lengths are not supported.

## Indexing and length

`values.len()` returns `usize` and takes no arguments. The receiver still
evaluates once, so `make().len()` executes `make()`. Initialized array temporaries
can also be indexed for reading, but cannot be assigned through or borrowed.

Indices must have type `usize`. Literals get that context; already typed
integers require an explicit cast. Literal and simple constant arithmetic
indices known to exceed the length are rejected at compile time. No index
is valid for a zero-length array. Other indices receive an unsigned runtime
bounds check before address derivation or element access.

Invalid runtime accesses execute `llvm.trap` and terminate the program. This
also applies to writes and taking an element's address. Unsafe blocks do not
disable bounds checks; unchecked array operations are not exposed. Bounds
failure does not return an error value or unwind scope cleanups.

Indexing captures its receiver first, then evaluates its index once. For a
store, bounds checking finishes before the right-hand side evaluates; the
right-hand side also evaluates once. Nested indexing checks every dimension.

## Const and borrowing

```flecha
const [2]u8 fixed = [1, 2];
// fixed[0] = 3; // error
// fixed = [3, 4]; // error
copy := fixed;
copy[0] = 3; // the copy is mutable
```

Const arrays prohibit whole-value replacement and element/subarray mutation.
Const does not propagate through raw pointers to their pointees. `[N]const T`,
including element-level `constptr`, is reserved; use `const [N]T` for a read-only
array. Pointee qualifiers such as `[N]*const T` remain supported.

```flecha
void change(&[2]u8 values) {
    (*values)[0] = 42;
}

u8 example() {
    [2]u8 values = [1, 2];
    element := &values[1];
    *element = 3;
    change(&values);
    return values[0];
}
```

Array and element references reuse the existing qualifier, move, reborrow, and
lifetime rules. A borrow of any element or nested subarray protects the entire
root array. Two overlapping mutable element borrows are rejected even if their
indices differ; shared element borrows may coexist. Reads of the array,
including `.len()`, follow the conservative whole-owner access checks.

A child reborrow freezes conflicting parent access until its last possible
use. A reference used to address an element cannot be moved or rebound while
its index or store value is being evaluated. Borrowed elements cannot escape
their array's lifetime; borrowing an array temporary is rejected. Raw array
pointers still require unsafe permission for dereference and cannot establish
safe references by themselves.

## Diagnostics and validation

| Code | Meaning |
| --- | --- |
| `FLECHA-S0033` | Array length/layout exceeds supported limits |
| `FLECHA-S0034` | Unsupported array element type or element const qualifier |
| `FLECHA-S0035` | Initializer length does not match the expected array length |
| `FLECHA-S0036` | Statically invalid array index |

Existing type, initialization, const, borrow, and unsafe diagnostics also apply.
`make test` includes parser/range/recovery tests, semantic and flow regressions,
bound IR and LLVM verification, and `NativeSemanticTests.FixedArrays`. Native
coverage checks copies, returns, nested/empty arrays, side-effect order, and
bounds traps for reads, writes, borrows, nested indices, and unsafe accesses.
The sanitizer target includes the compiler and native harness/runtime checks.

Arrays lower to LLVM array values and native addresses, with entry-block
storage for expression temporaries. Repeats use a counted loop rather than
expanding the source initializer per element. The aggregate function calling
convention is internal to Flecha; C interoperability for by-value arrays is not
yet a public ABI contract.

Borrowed [checked slices](checked_slices.md) are implemented. Resizing,
heap-backed arrays and partial initialization/moves remain separate features.
Arbitrary field or indexed-element moves are rejected; only the complete array
can move. Direct arrays of reference, slice, string, or ownership-qualified raw
pointer elements remain unsupported, although supported structures may manage
their own fields and lifecycle.

## Iteration

Array ranges and counted loops are implemented; see [iteration.md](iteration.md):

```flecha
for (index, value) := range values {}
for (u32 index, i32 value) = range values {}
loop usize index = 0; index < values.len(); index++ {}
```

Range values are copies from a snapshot evaluated once before the loop. Use the
counted form or index-only range with explicit indexing to mutate the original.

## Structure elements

Arrays also support [structure](structures.md) elements. Their fields are
constructed in place and destroyed in reverse element order. Copyable elements
clone owned fields; arrays containing a safe custom tear are noncopyable but
move as whole values. An unsafe tear makes the containing array immovable. Such
arrays require an initializer and cannot duplicate an existing element in a
repeat with length greater than one. These rules also apply when arrays are
structure fields.
