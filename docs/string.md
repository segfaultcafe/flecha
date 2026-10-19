# Strings

Flecha has two complementary text types:

- `stringv` is an immutable, non-owning view of bytes.
- `string` is an immutable value that owns its backing allocation.

The initial string model is byte-oriented. Lengths and indices count bytes,
and `char` has the same representation as `u8`. A `char` converts implicitly to
`u8`, including in byte-array initializers and function arguments. The reverse
conversion remains explicit. Unicode decoding and code-point iteration belong
in a later text library and do not change the storage contract described here.

## Literals and ownership

A string literal has type `stringv` and refers to static immutable storage:

```flecha
view := "flecha";              // inferred stringv
string owned = "flecha";       // creates an owning copy
```

An owning string stores a data pointer, byte length, and capacity. Its runtime
storage is null-terminated for interoperability, but the explicit byte length
is authoritative and does not include the terminator. Embedded null bytes are
valid.

## View-to-string conversion

Converting a view to `string` allocates storage and copies its bytes. Both the
general cast syntax and an idiomatic member are supported:

```flecha
stringv view = "flecha";

string explicitOwned = view.cast<string>;
string idiomaticOwned = view.string();
```

`view.string()` is preferred in normal Flecha source because it makes the
creation of an owning value conspicuous. `cast<string>` remains available for
generic conversion code and preserves the language's chainable cast model.
The two forms have identical semantics and diagnostics.

Converting `string` to `stringv` is a non-allocating borrow:

```flecha
string owned = "flecha";

stringv explicitView = owned.cast<stringv>;
stringv idiomaticView = owned.view();
```

`owned.view()` is the idiomatic spelling. `cast<stringv>` exists for symmetry
with `stringv.cast<string>` and for generic conversion code. Both forms create
the same immutable borrow and do not allocate or copy bytes.

The view must not outlive or observe destruction or replacement of its owner.
The compiler conservatively rejects returning or otherwise escaping a view
derived from a local owning string.

## Initial operations

Both string types provide byte-based inspection:

```flecha
text.len();              // u32
text.at(0);              // checked char
text[0];                 // checked char, equivalent to at(0)
text.substr(0, 3);       // allocation-free stringv
```

Unsafe runtime bridges may also call `text.data()` to obtain a borrowed
`*const char`. The pointer is paired with the explicit byte length, may be null
for an empty value, and must not be retained beyond the string or view's
lifetime. Extern calls may pass it to a `*const u8` parameter without an
additional pointer operation. This operation is intentionally unavailable in
safe code.

Following Flecha library API style, member names use `camelCase`. Existing
single-word members such as `len`, `at`, and `substr` already follow that rule;
multiword members use spellings such as `startsWith` and `isEmpty`.

Concatenation creates a new owning value:

```flecha
string message = "hello, " + name;
```

It accepts `string` and `stringv` operands. Neither operand is modified.

## Value and cleanup semantics

The first implementation uses predictable value semantics:

- Copy initialization and assignment produce an independent owned value.
- Assignment destroys the destination's previous allocation after the new
  value has been created successfully.
- A `string` parameter receives an owned value.
- Returning a `string` transfers the result to the caller.
- `stringv` never destroys storage.
- Every owned local is destroyed exactly once on normal exit, `return`,
  `break`, `proceed`, and `jumpto` edges that leave its lifetime.

Move analysis may later remove unnecessary copies without changing observable
program behavior.

## Runtime responsibilities

The runtime provides versioned C ABI operations for:

- constructing an owned string from `stringv`;
- cloning an owned string;
- exposing an owned string as a non-owning `stringv` in generated code;
- concatenating string values;
- destroying owned storage;
- reporting allocation failure without allowing C++ exceptions across the
  ABI boundary.

Bounds checking for `at`, indexing, and `substr` follows the existing
`stringv` policy: an invalid dynamic range reports a runtime error and
terminates safely.
## Lifetime safety

`stringv` never owns storage. A view may be returned when it refers to static
literal storage or a borrowed `stringv` parameter, but a view into any owning
`string` in the current function cannot escape the owner's scope. The semantic analyzer reports
`FLECHA-S0020` when a returned view would outlive its local owner.

Owning `string` locals are destroyed at their lexical scope edge, including
control-flow exits through `return`, `break`, `proceed`, and `jumpto`.

Cleanup includes every owning slot in an exited scope, even when a backward
jump reaches an exit before that slot's declaration in source order. Slots
start null at function entry and are cleared after destruction. Temporary
storage used to transfer ownership is also cleared after the transfer, so
repeated declarations and assignments in loops cannot free the same allocation
twice. See [definite assignment](definite_assignment.md) for initialization
rules across positions and scope boundaries.
