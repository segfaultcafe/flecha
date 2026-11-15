# Typed decorations and library behaviors

In Flecha 0.4, a decoration declares compile-time application contracts. The
compiler validates targets and arguments; a library can attach a behavior that
implements declared interface methods for decorated structures or enums.

```flecha
export decoration json {
    application(structure) target structure;
    application(stringv name) target structure::field;
};

@json(structure)
structure Response {
    i32 value;
    @json("display-name") string name;
    private string secret;
};
```

`json` is an ordinary decoration name. The declaration above supplies metadata;
methods and conformance require a matching library-defined behavior. Neither
that name nor methods named `json`, `writeJson`, `readJson`, or `fromJson`
receive special treatment from the compiler.

Applications may use an imported module alias. An exported `json` decoration
from a module imported as `json` is available as `@json`;
`@annotations::json` explicitly selects a differently named module alias.
The compiler checks visibility, logical module ownership, duplicate
applications, argument types, and declared target categories.

## Contracts and activation

A behavior has exactly one unconstrained target parameter and belongs to the
same logical module as its decoration. At most one behavior can target each
concrete kind for that decoration:

```flecha
behavior json<T> => structure << JsonWritable, JsonReadable {
    // Implement receiver methods from the declared interface graph.
};
```

Structure applications and direct field applications activate the structure
behavior once per concrete owner. Enum applications activate enum behaviors.
Interface decorations are metadata only. Files attached to the same logical
module share ownership; importing a decoration does not let a consumer declare
its behavior in a different module.

Active behaviors must satisfy all virtual requirements. Exact handwritten
`override` providers take precedence over behavior bodies, and inherited
interface defaults/finals follow ordinary interface rules. Static factories
are ordinary library functions, such as `json::from<Response>(source)`.

## Bounded reflection

`for<F> Field<T,F> field = T::fields()` expands in source declaration order with
the exact field type bound to `F`. `field::decorated()`, `field::decorator()`,
`field::name()`, and `field::isPrivate()` expose typed compile-time metadata;
`field::get(self)` projects the concrete field with ordinary access, const,
ownership, and borrow checks.

Libraries express their own selection and validation policies:

- `T::require(condition, message)` and `field::require(condition, message)`
  report compile-time requirements with source provenance.
- `field::requireUnique(key)` rejects duplicate constant keys among selected
  fields in one lexical field iteration.
- Direct `F.isof<...>` branches select compatible bodies before ordinary type
  checking. `F.isof<own []E>` binds the owned array's element type in the
  selected branch, including nested owners.
- `proceed` skips one expanded field; `break` exits the field iteration.
  Nested runtime loops retain their nearest-loop transfer targets.
  `jumpto` remains prohibited in behavior bodies.

Expansion publishes methods and conformance only after validation succeeds.
Field descriptors, metadata, and reflection operations have no runtime
representation. See [the behavior contract](behaviors-v0.4.md) for the complete
rules and diagnostic identifiers.

## Migration from compiler-owned JSON

The v0.3 form `export decoration json;` no longer activates compiler-generated
serialization. An application of a bodyless decoration reports that it
requires a typed application contract. Update to the Flecha behavior-based
JSON library; merely changing the declaration does not supply implementations.

Public-field selection, explicit field selection, renamed keys, duplicate-key
validation, reader/writer composition, and supported field types are library
policies. The compiler no longer generates JSON methods, resolves Reader or
Writer by convention, or provides `__json_slice_append`.

`value.json()`, `value.writeJson(&writer)`, and `value.readJson(&reader)` are
provided through the library's interfaces and behavior. Replace the old
`Response::fromJson(source)` convenience factory with
`json::from<Response>(source)`. Handwritten overrides must match the library's
interface contracts, as with any other behavior.

See [JSON library migration](json-library-migration.md) for compatibility and
verification. The historical 0.3 contract remains documented in the
[0.3 release notes](release_0_3.md#temporary-json-compiler-contract).
