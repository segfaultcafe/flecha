# Tuples, multiple results, and destructuring

Tuples are positional values with two or more elements. A type lists element
types, and a value lists expressions in the same order:

```flecha
(i32, bool) result = (42, true);
inferred := (42, true);
(i32, (bool, u8)) nested = (42, (true, 7));
```

Parentheses around one expression remain grouping: `(42)` is an `i32`.
Empty tuples and single-element tuples are not supported. A trailing comma is
allowed after two or more elements. Elements evaluate exactly once, left to
right. Each element's value is captured before evaluating the next element.

See the runnable [example](../examples/tuples.fl).

## Functions

Tuple types work as return types and parameter types:

```flecha
(i32, i32) divide(i32 dividend, i32 divisor) {
    return (dividend / divisor, dividend % divisor);
}

(i32, i32) pass((i32, i32) value) {
    return value;
}
```

There is one tuple result; returning multiple values does not mean evaluating
multiple independent calls. Recursion, forwarding results, and ordinary tuple
variables all use the same representation. Contextual typing applies separately
to elements, including narrow integer literals and string conversions. Already
typed incompatible integers still need explicit casts.

## Destructuring

```flecha
a, err := divide(17, 5);           // Declare both names with inferred types.
next, err := divide(20, 3);        // Declare next and assign the existing err.
i32 q, i32 r = divide(17, 5);      // Declare both names explicitly.
i32 quotient, remainder = divide(17, 5); // Shared i32 type.
a, b = (b, a);                    // Assign existing names; swap their values.
a, i32 next = divide(20, 3);       // Assign a; declare next.
_, b = divide(19, 5);             // Discard an element.
```

- `:=` declares each new non-blank binding and assigns names already declared
  in the same lexical scope. At least one non-blank binding must be new. Names
  from an outer scope are shadowed rather than assigned. Explicit types are not
  allowed in a `:=` binding list.
- Without parentheses, an explicit type applies to the following names until
  another type is written. `bool ok, changed, i32 code = ...;` declares two
  booleans and an integer. Qualifiers and compound types carry forward too.
  These names are declarations even if a same-named variable already exists;
  ordinary shadowing and duplicate-declaration rules apply.
- With `=`, a name with no preceding type in the list assigns an existing
  variable. `_` discards its corresponding element without declaring or
  looking up a name; it may appear more than once.
- Parenthesized binding lists remain supported. Within them, each explicit
  type applies only to its own binding, preserving mixed declaration/assignment:
  `(i32 fresh, existing) = value;`. Tuple types, tuple values, and function
  result types still use parentheses.
- The entire right-hand side evaluates once before any destination store.
  New bindings become visible after the right-hand side. Existing values are
  therefore available when shadowing a name in an inner scope.
- The number of bindings must match the tuple's arity. Repeated non-blank
  destination names, incompatible types, and writes to const bindings are errors.
- Destructuring participates in definite assignment and borrow checking.
  Stores do not initialize names early enough for reads in the right-hand side,
  and they cannot mutate owners with conflicting active loans.
- Destination patterns are flat lists of names. To unpack a nested tuple,
  bind it to a name and destructure that name in another statement.

Destructuring is currently a standalone statement. Indexed/dereferenced targets,
nested patterns, tuple element access syntax, and tuple comparisons are not
introduced in this milestone.

## Values, qualifiers, and ownership

Supported elements are numeric scalars, booleans, characters, raw pointers,
owning strings, error values, structures, supported fixed arrays, and nested
tuples. A tuple is copyable when all its elements are copyable. Those tuples
use value-copy semantics; owning strings are deep-copied whenever ownership is
duplicated, including in deferred snapshots. A tuple containing a noncopyable
but movable structure instead moves as a complete value on initialization,
assignment, by-value argument or return, discard, and complete destructuring.
Its source becomes unavailable until reassigned. Discarded or ignored owning
elements are destroyed exactly once.

An expected `string` element accepts a string literal or view by copying it:

```flecha
(string, i32) named = ("hello", 42);
(string name, i32 number) = ("hello", 42);
owned := ("hello".string(), 42);
```

Inferred `("hello", 42)` would contain `stringv`, so it is rejected. References
and slices may appear in a flat function-result tuple covered by an explicit
`<- parameter>` contract, but that result must be immediately destructured;
see [reference and slice returns](ref_and_slice_returns.md). Stored borrowed
tuples, nested borrowed tuples, and string-view tuple elements remain rejected.
Arrays/slices of tuples are also deferred. Raw pointers retain their unsafe
lifetime contract.

`const (i32, bool)` fixes the tuple binding. Reading or destructuring a const
tuple copies its values into independently writable inferred bindings; explicit
`const` bindings remain fixed. References to tuples without owning strings can
borrow and replace the whole value. References to owning tuples remain
unsupported, consistent with the existing restriction on borrowing strings.

`none` zero-initializes an explicitly typed tuple without structures recursively,
including empty owning strings and null raw pointers. Tuples containing structures
require construction; their copy, destruction, and immovability rules follow
[structures](structures.md). Complete noncopyable tuples can be moved and
destructured; moving an individual tuple field is not supported. A tuple that
contains an unsafe-tear value is immovable. Delayed tuple initialization uses
the existing whole-variable definite-assignment rules. Tuples cannot be cast.

## Cleanup and ABI

Returning a copyable owning tuple produces an independent result; returning a
noncopyable movable tuple transfers it. Local tuples, parameters, temporaries,
and deferred captures recursively release only the ownership they retain.
Existing cleanup edges cover returns, fallthrough, break, proceed, and outward
jumps, including repeated declarations reached by backward jumps. Traps retain
the existing no-unwind behavior.

The internal LLVM representation is a non-packed struct whose fields follow
source order. Nested tuples remain nested structs. Results containing a
structure use caller-provided storage; structure-free tuples use LLVM aggregate
values. Padding follows the target data layout. This is an internal Flecha ABI,
not a promised C struct ABI or stable cross-version binary interface. No foreign
aggregate interoperability is introduced.

Errors use `FLECHA-S0039` for unsupported tuple contents/layout and
`FLECHA-S0040` for invalid destructuring. Ordinary type, initialization,
mutability, and borrow failures retain their existing diagnostics.

Named and generic [error values](error_values.md) work as tuple elements, with
implicit named-to-generic widening. Allocator APIs and propagation sugar remain
separate later features.
