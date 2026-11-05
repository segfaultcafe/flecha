# Const declarations and compound types

The first milestone implemented const value declarations and the recursive type
foundation for pointers and references. Raw pointers are now executable as
described in [unsafe contexts and pointers](unsafe_pointers.md). Checked scalar and pointer-slot references are described in
[checked references](checked_references.md).

## Const values

```flecha
const i32 add(const i32 value) {
    const i32 increment = 2;
    return value + increment;
}

i32 main() {
    const i32 answer = add(40);
    copy := answer;
    copy = copy + 1;
    return copy - 1;
}
```

- A const local of trivially zeroable type may omit its initializer; this is
  shorthand for `= none`. Other const locals require an explicit initializer,
  which may execute at runtime.
- Const parameters receive their initial value from the caller.
- Assignment to either is `FLECHA-S0005`, pointing at the assignment target.
- Invalid initializer omission is reported at the local's name.
- Reads produce values without the outer binding qualifier. Copies and inferred
  declarations remain mutable unless their own declaration says `const`.
- Top-level const on return and cast types does not make the receiving binding
  const. It does not change the native ABI.
- Explicit `none` literals use the destination type's zero value in
  declarations, assignments, function arguments, and returns. This applies to
  mutable and const destinations alike.
- Scope resolution and definite assignment still apply, including shadowing and
  jumps that skip a declaration's initializer.
- `string` retains its owning copy behavior; `stringv` still requires an
  initializer and must not outlive its owner. Const does not extend lifetimes.
- Only typed const declarations are implemented; `const name := value` is not
  part of this milestone.

## Module constants

At module scope, a typed const declaration is a compile-time constant rather
than a runtime local:

```flecha
module buffer;

const usize INITIAL_CAPACITY = 8;
export const usize LARGE_CAPACITY = INITIAL_CAPACITY * 4;
```

Its initializer may contain literals, other module constants, unary and binary
operators, casts, `isof`, numeric limits, floating-point constants, enum
variants, and error values. It cannot call functions, allocate storage, refer
to runtime values, or participate in a dependency cycle. Module constants
support numeric, `bool`, `char`, `stringv`, enum, and error-value types.

All files contributing to one logical module share its constants. Unmodified
constants remain module-private; `quiver` and `export` apply the ordinary
top-level visibility rules. A use is lowered as the immutable constant
expression, so this feature introduces no mutable global initialization.

## Recursive types and aliases

| Spelling | Normalized type | Fixed binding | Read-only target |
| --- | --- | --- | --- |
| `*u8` | `*u8` | No | No |
| `*const u8` | `*const u8` | No | Yes |
| `constptr u8` | `const *u8` | Yes | No |
| `constptr const u8` | `const *const u8` | Yes | Yes |
| `&u8` | `&u8` | No | No |
| `&const u8` | `&const u8` | No | Yes |
| `constref u8` | `const &u8` | Yes | No |
| `constref const u8` | `const &const u8` | Yes | Yes |

Each type layer has its own const qualifier. `&constptr u8` normalizes to
`&const *u8`; a reference to a raw pointer is representable. Adjacent reference
markers use whitespace (`& &u8`) to distinguish them from the logical `&&` token.
`const constptr u8` and `const const u8` duplicate a qualifier on one layer and
are rejected. `constval` is not reserved.

Syntax nodes retain each layer's full source range and the range of its const
keyword (or alias). Semantic types compare recursively by kind, qualifier, and
target, rather than by pointer identity. Diagnostics print expanded type names.
Symbols and function signatures retain declared qualifiers; bound value
expressions omit only the outer qualifier. Nested qualifiers are never erased
by this value-copy normalization.

## Current boundary

Scalar and raw-pointer storage can be borrowed with `&value`; see
[checked references](checked_references.md) for qualifier, move, and lifetime
rules. Unsupported reference storage shapes emit `FLECHA-S0024`.
Raw pointers use the explicit `&raw value` operation inside unsafe contexts;
see [unsafe pointers](unsafe_pointers.md) for executable operations and limits.

Allocation, free, defer, general owner moves, slices, and volatile access
remain later work in the [implementation plan](memory_implementation_plan.md).

## Validation

`make test` covers lexing, recursive parsing and ranges, aliases, type equality,
const diagnostics, lowering, LLVM verification, and the native
`NativeSemanticTests.ConstDeclarations` fixture. The fixture exercises runtime
initializers, parameters, scalar casts, shadowing, independent owning string
copies, and borrowed views; it prints `initialized` and exits with 42.

`make test-sanitizers` runs the same native fixture with the instrumented runtime
and leak detection. `make test-neovim` checks the added keywords and `&` marker.
