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
derivation proof of concept. It builds deterministic serialization metadata
and generates methods against the `Writer` and `Reader` structures exported by
the decoration's module. JSON syntax, buffering, and parsing remain library
work; the compiler only derives structure traversal.

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
plan and serializes as `{}`.

## Generated JSON methods

When the decoration module exports `Writer`, each decorated structure receives
these methods:

```flecha
error Response::writeJson(&const self, &Writer writer);
(own []u8, error) Response::json(&const self);
```

`writeJson` composes nested values into an existing writer. `json()` creates a
writer, calls `writeJson`, and returns `writer.finish()`. Either method may be
replaced by an exact `override`.

When the same module also exports `Reader`, the compiler additionally derives:

```flecha
error Response::readJson(&self, &Reader reader);
(Response, error) Response::fromJson([]const u8 source);
```

`readJson` consumes one object from an existing reader, dispatches fields by
their selected JSON keys, recursively reads decorated structures, and skips
unknown fields. `fromJson` zero-initializes the result, calls `readJson`, and
then requires `reader.finish()` so trailing input is rejected. Both methods
also support exact overrides. `fromJson` is an ordinary receiver-less
structure function; it introduces no new language modifier.

Bidirectional derivation supports booleans, every integer and floating-point
primitive, owning `string`, and recursively decorated structures that use the
same Reader and Writer. A `stringv` field cannot be derived for reading because
decoded JSON text needs owned storage.

The following are compile-time errors:

- applying `@json` to a declaration other than a structure or structure field;
- decorating a private field for serialization;
- passing anything other than `default`, `explicit`, or no arguments to
  structure-level `@json`;
- passing anything other than one string key to field-level `@json`; and
- selecting the same JSON key more than once;
- selecting a field without a supported JSON representation; and
- using `stringv` with a Reader-backed decoration.

Arbitrary AST mutation and host-executed procedural decorations remain outside
this design. This keeps builds deterministic and prevents a dependency from
gaining code-execution privileges merely because it exports a decoration.
