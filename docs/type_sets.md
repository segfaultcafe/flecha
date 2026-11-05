# Type sets

Type sets are closed compile-time collections of types. They constrain explicit
generic specializations without creating runtime supertypes, tagged unions,
interfaces, or dynamic dispatch.

```flecha
T identity<T: numeric>(T value) { return value; }

identity<i32>(42);    // valid
identity<f64>(1.5);   // valid
identity<bool>(true); // error: bool does not satisfy numeric
```

The compiler checks the constraint before specializing the declaration. It then
checks the specialized body normally, so membership does not grant operators or
otherwise bypass semantic analysis.

## Built-in sets

The compiler provides these closed sets:

```text
numeric
├── integer
│   ├── signed_integer: i8, i16, i32, i64, isize
│   └── unsigned_integer: u8, u16, u32, u64, usize
└── floating_point: f32, f64
```

`bool`, `char`, enums, pointers, references, arrays, slices, tuples, strings,
and structures are not implicit members. Top-level `const` does not change
membership. Integer-backed enums remain nominal types rather than integers.

Built-in set names are recognized only in a generic constraint, an `isof`
query, or as a nested member of another type set. They are not value types and
cannot be used to declare storage.

## Custom sets

Declare a custom set with `typeset`. Members may be concrete primitive types,
nominal types, concrete generic specializations, built-in sets, or other custom
sets:

```flecha
structure UserId { u64 value; };
structure Box<T> { T value; };

typeset identifier {
    integer,
    UserId,
};

typeset accepted_value {
    identifier,
    f64,
    Box<u32>,
};
```

Nested membership is transitive. Type-set cycles are invalid. Nominal members
match by exact type identity, and generic members match the exact ordered
specialization; `Box<u32>` does not include `Box<u64>`.

Type-set declarations follow ordinary module visibility. `export` exposes a set
to every importer, `quiver` exposes it within the same namespace directory, and
an unmodified declaration remains module-private. A visible set cannot expose a
less-visible member type or nested set. Different modules may define sets with
the same short name; normal import alias and overlap rules apply.

## Membership expressions

`expression.isof<type_or_set>` produces a compile-time `bool` from the static
type of its operand:

```flecha
bool uses_integer_representation<T>() {
    return T.isof<integer>;
}

bool examples() {
    return 3.isof<i32> and 3.isof<numeric> and !3.isof<f64>;
}
```

The expression is replaced with `true` or `false` during specialization and has
no runtime type metadata or membership test. The target may be a concrete type
or a visible built-in or custom type set. The operand is used only for its
static type and is not evaluated at runtime.

## Scope

Type sets express closed identity membership. They do not express capabilities
such as “supports addition,” allow a type to implement a set after its
declaration, or provide method requirements. Those behaviors would require a
future trait or protocol design.
