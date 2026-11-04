# Declarative decorations

Decorations attach compile-time metadata to declarations without executing
package code inside the compiler. The initial surface deliberately supports
declarations and structure fields only:

```flecha
export decoration json {};

@json(default)
structure Response {
    i32 value;
    @json("test") string testMe;
    private string secret;
};
```

A decoration declaration has an empty body. Its body is reserved for a future
declarative target and argument schema; it is not a procedural macro body.
Applications accept identifier and string arguments and may use an imported
module alias. An exported `json` decoration from a module imported as `json`
is therefore available as `@json`, while `@annotations::json` selects a
differently named module alias explicitly.

The compiler resolves every applied decoration and rejects unknown,
inaccessible, or duplicate applications. Decorations are preserved when a
generic declaration is specialized. Decoration expansion performs no file,
environment, network, clock, or host-process access.

## JSON proof of concept

The compiler recognizes a resolved decoration named `json` as the first
derivation proof of concept. It builds deterministic serialization metadata;
it does not yet generate a serializer or make JSON part of the standard
library.

`@json(default)` on a structure selects every public field in declaration
order. A field-level `@json("name")` overrides that field's JSON key. Private
fields are skipped:

```flecha
decoration json {};

@json(default)
structure SerializedDefault {
    i32 value;
    string testMe;
    private i32 cachedHash;
};
```

A structure without a structure-level application enters explicit mode when
one of its fields uses `@json("name")`. Only decorated public fields are then
selected:

```flecha
structure Response {
    @json("value") i32 value;
    @json("test") string testMe;
    private string secret;
};
```

An unargumented structure-level `@json` also selects explicit mode. A
default-mode structure containing only private fields produces an empty field
plan and will serialize as `{}` once serializer generation is connected.

The following are compile-time errors:

- applying `@json` to a declaration other than a structure or structure field;
- decorating a private field for serialization;
- passing anything other than `default` to structure-level `@json`;
- passing anything other than one string key to field-level `@json`; and
- selecting the same JSON key more than once.

## Planned derivation boundary

The next increment will expose the validated field plan to generated methods
or a constrained library derivation interface. Arbitrary AST mutation and
host-executed procedural decorations remain outside this design. This keeps
builds deterministic and prevents a dependency from gaining code-execution
privileges merely because it exports a decoration.
