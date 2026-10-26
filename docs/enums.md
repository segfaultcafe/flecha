# Enums

Enums are nominal, allocation-free scalar value types. Each enum chooses an
integer backing type, declares a closed set of named variants, and may select
one variant as its semantic default.

## Declaration and discriminants

```flecha
export enum TokenKind: u16 {
    default NoToken = 0,
    Identifier,
    Number = 10,
    End,
}
```

The backing type is optional and defaults to `u32`. Any mutable signed or
unsigned integer type is accepted. The first implicit discriminant is zero;
later implicit discriminants are one greater than the preceding value. Explicit
discriminants may be positive or, for signed backings, negative. Values must fit
the backing type, and variant names and discriminant values must be unique. A
trailing comma is optional. Unlike a structure declaration, an enum declaration
does not end in `;`.

An enum name is a type, not a value. Variants are qualified with the enum name:

```flecha
TokenKind kind = TokenKind.Identifier;
```

Enums are nominal. Two enums with the same backing type and discriminants are
still different types, and there are currently no implicit or explicit
integer-to-enum or enum-to-integer conversions.

## Defaults and comparison

At most one variant may have the contextual `default` marker. When present,
`none` constructs that variant in declarations, assignments, arguments,
returns, aggregates, and ordinary equality comparisons:

```flecha
enum State: u8 {
    Off,
    default Unknown = 7,
    On,
}

State state = none;             // State.Unknown
[2]State states = none;         // both elements are State.Unknown
if state == none {}             // compares with State.Unknown
if state === none {}            // always false: the static types differ
```

Without a default variant, contextual `none` is rejected for that enum,
including when it is nested in an array or tuple. A variable may still be
declared without an initializer, but definite-assignment rules prevent it from
being read before assignment.

`==`, `!=`, `===`, and `!==` are supported only between values of the same enum
type. Arithmetic, ordering, increment, implicit truth tests, and comparisons
between different enums are rejected.

## Member functions

Enum functions use the same out-of-line receiver model as structure methods:

```flecha
bool State::is_on(&const self) {
    return *self == State.On;
}

void State::turn_on(&self) {
    *self = State.On;
}

State State::initial() {
    return State.Unknown;
}
```

Value receivers use `.`, reference and pointer receivers use `->`, and static
functions use `State::initial()`. The first parameter may be `&self`,
`&const self`, `*self`, `*const self`, or the consuming value receiver `self`;
checked-reference and unsafe-pointer rules are unchanged. Methods may be
`private`, and exporting an enum exposes its non-private methods. Methods may
declare explicit generic parameters. A method and variant cannot share a name.

Enum initialization and destruction are compiler-defined. `init`, `zero`, and
`tear` are reserved and cannot be declared as enum methods.

## Storage and representation

An enum lowers to its backing integer representation and is copyable. Enums may
be stored in structures, fixed arrays, tuples, generic values, references, and
raw pointers. A structure field or runtime-sized array element that requires
default construction needs the enum to declare a default variant. Non-zero
defaults are emitted as semantic initialization rather than bytewise zeroing.

The numeric representation is an implementation detail of the declared enum;
enum types are not currently permitted in `extern` signatures. Enums can be
inspected with exhaustive `match` expressions. See
[enum matching](match.md) for patterns, wildcard arms, result typing, and the
current restrictions.
