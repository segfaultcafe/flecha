# Array ranges and counted loops

Implemented with fixed arrays. `for`, `loop`, and `range` are reserved keywords.

## Array range

```flecha
[3]i32 values = [10, 20, 30];
for index := range values {}
for (index, value) := range values {}
for (u32 index, i32 value) = range values {}
for (_, value) := range values {}
```

One binding receives the index. Two bindings receive index, then element value,
following [Go's range binding order](https://go.dev/ref/spec#For_range).
Inferred declarations use `:=`; explicit declarations use `=` and specify both
types in the two-binding form. `T` in a syntax description means the actual
array element type, such as `i32`; it does not introduce generic type syntax.
These forms declare fresh loop-local bindings; `=` does not reassign existing
variables. `_` discards either binding. Explicit discarded types are still
checked.

The inferred index type is `usize`. Explicit indices may use any integer type
that represents every index from zero through `N - 1`. For example, `u8` can
index a 256-element range but not a 257-element range. The internal cursor stays
`usize`, so incrementing after index 255 does not wrap the loop. Explicit element
types must accept the array's element type under ordinary assignment rules;
there is no implicit numeric conversion.

The array expression evaluates once before the loop, and its value is copied
into private storage. This also happens for empty arrays and index-only ranges;
Flecha does not adopt Go's exception that can omit evaluating a range expression
when only constant length information is needed. Iteration proceeds in ascending
index order. Each iteration declares fresh index/value bindings. Assigning these
bindings does not affect the array or the iteration cursor. Changes to the
original array do not change the snapshot. Nested array elements also copy by
value; pointer elements copy addresses without owning the pointees.

Fixed-array and slice values are supported. Use `range *arrayReference` to
copy through a reference. Slice ranges take a shared reborrow for the duration
of the loop, without copying the backing elements; see [checked_slices.md](checked_slices.md).
Slice indices must use `usize`, including explicit declarations. Strings, numeric
ranges, and implicit iteration through a pointer/reference are not introduced here. Existing initialization and
borrow checks apply to the source read. References to an iteration binding cannot
escape that iteration's scope.

## Counted loop

```flecha
loop usize i = 0; i < values.len(); i++ {
    if i == 1 { proceed; }
    values[i]++;
}
```

The initializer executes once; the condition is tested before every iteration;
the update executes after the body. `proceed` executes the update before testing
the condition. `break` exits without updating. Nested loop control applies to
the innermost loop. `return`, positions, and `jumpto` retain their existing scope,
cleanup, and initialization rules.

The initializer can be a declaration or expression. Its declarations are visible
in the condition, update, and body, and end at the end of the loop. Body locals
are unavailable in the update. The condition must have type `bool`. Any clause
may be omitted, but both semicolons remain; an omitted condition means `true`:

```flecha
loop ;; { break; }
```

Array indexing uses `usize`, starts at zero, and requires `i < values.len()`.
An inclusive comparison such as `x <= 10` remains useful for numeric counters.
The new loop forms do not have trailing `also` clauses; existing `while`/`also`
syntax is unchanged.

## Postfix and compound updates

`place++` and `place--` require an initialized, writable integer location. They
evaluate the location once, update it by one, and yield the previous value.
Variables, array elements, and dereferenced references/pointers are supported.
Const, borrow, bounds, and unsafe-context checks apply. Raw-pointer arithmetic
and prefix `++`/`--` are not included. Integer overflow wraps at the integer
width, matching ordinary integer addition and subtraction.

```flecha
old := values[next_index()]++; // one index call; checked before load/store
previous := values[next_index()]--;
(*reference)++;
unsafe { (*pointer)++; }
```

Compound assignments `+=`, `-=`, `*=`, and `/=` accept matching numeric
operands. `^=` accepts matching integer or Boolean operands. Each operator
evaluates its target location once, stores the updated value, and yields that
new value.

```flecha
values[next_index()] += 2; // one index call
total *= scale;
flags ^= mask;
```

## Compiler representation and coverage

The parser normalizes counted loops into a scoped initializer and a while node
with a separate optional update. Range nodes retain their source and binding
metadata for type/capacity diagnostics while owning a normalized snapshot,
cursor, and loop. Internal identifiers contain `$`, which cannot occur in source
identifiers. Generated range element accesses are guarded by their loop condition,
including the unreachable body of a zero-length range; user indexing keeps its
ordinary static checks. Lowered element accesses still use runtime bounds checks.

The shared control-flow graph routes `proceed` through the update and keeps
iteration locals in their own scope. Native tests cover one-time evaluation,
snapshot copies, empty/nested ranges, narrow indices, update-target evaluation,
compound arithmetic, nested loop control, returns/jumps, and owning-string
cleanup on each exit.
