# Declarative decorations

Decorations attach compile-time metadata. In v0.3, a decoration declaration
only introduces a name that the compiler already knows how to interpret. It
does not define targets, arguments, validation, generated methods, or any
other behavior in Flecha source.

The JSON library deliberately keeps an empty public declaration:

```flecha
export decoration json;
```

Applications may use an imported module alias. An exported `json` decoration
from a module imported as `json` is available as `@json`, while
`@annotations::json` explicitly selects a differently named module alias. The
compiler resolves every application and rejects unknown, inaccessible, and
duplicate decorations.

## v0.3 declaration rule

Every decoration that can be used in Flecha 0.3 must have a compiler
counterpart. Declaring a decoration by any supported source form does not make
it programmable: the matching compiler implementation owns its permitted
targets and arguments, diagnostics, and effects. A library may export the
declaration so consumers can resolve the name, but it cannot add a new
decoration behavior without a coordinated compiler change.

This rule applies to JSON and to any other decoration introduced during the
0.3.x line. The source declaration is an identity and visibility boundary,
not a macro, plugin, callback, or executable compile-time body. Library-owned
decoration behavior is a post-v0.3 feature.

## JSON contract in v0.3

JSON is the one compiler-known decoration in Flecha 0.3. Its two application
forms are distinguished by their arguments and targets:

```flecha
@json(structure)
structure Response {
    i32 value;
    @json("test") string testMe;
    private string secret;
};
```

`@json(structure)` targets a structure and selects every public field in
declaration order. `@json("name")` targets a structure field, selects that
field, and replaces its serialized name. The latter is the concrete use of the
field-level `@json(string)` contract. Private fields are never serialized.

A structure without `@json(structure)` enters explicit selection when at least
one public field uses `@json("name")`:

```flecha
structure ExplicitResponse {
    i32 ignored;
    @json("value") i32 value;
    @json("test") string testMe;
    private string secret;
};
```

Only the decorated public fields are selected in that form. A structure-level
application containing only private fields produces an empty JSON object.

The JSON decoration module must export `Writer`. Every selected structure then
receives:

```flecha
error Response::writeJson(&const self, &Writer writer);
(own []u8, error) Response::json(&const self);
```

`writeJson` composes the structure into an existing writer. `json()` creates a
writer, calls the final generated or overridden `writeJson`, and returns
`writer.finish()`.

When the module also exports `Reader`, the structure receives:

```flecha
error Response::readJson(&self, &Reader reader);
(Response, error) Response::fromJson([]const u8 source);
```

`readJson` consumes one object, dispatches fields by their selected names,
recursively reads decorated structures, and skips unknown fields. `fromJson`
zero-initializes the result, calls the final generated or overridden
`readJson`, and requires `reader.finish()` so trailing input is rejected.

All four are default methods. A user may replace any one with an exact
`override`; the remaining generated methods call the replacement normally.
The compiler registers the complete generated and overridden signature set
before analyzing method bodies, so `json()` can resolve `writeJson()` and
`fromJson()` can resolve `readJson()` without body-based declaration inference.

Bidirectional derivation supports booleans, every integer and floating-point
primitive, owning `string`, and recursively decorated structures using the
same Reader and Writer. A `stringv` field cannot be derived for reading because
decoded JSON text requires owned storage.

The following are compile-time errors:

- applying `@json(structure)` to anything other than a structure;
- applying `@json("name")` to anything other than a public structure field;
- using any other JSON application argument shape;
- selecting the same serialized name more than once;
- selecting a field without a supported JSON representation; and
- using `stringv` with a Reader-backed decoration.

### Temporary compiler coupling

From Flecha 0.3.0 until the planned 0.4.0 behavior migration, the compiler
recognizes the resolved `json` decoration and owns its target validation,
field selection, name overrides, supported-type checks, and four default-method
implementations. The empty declaration is a public identity, not the source of
that behavior in v0.3.

This makes `flecha/json` tightly coupled to the matching compiler protocol for
the 0.3.x line. The exported `Writer` and `Reader` names, the methods called by
generated code, their signatures, ownership contracts, and observable
semantics must not change independently. Such a change requires a coordinated
compiler update. Private parser, buffering, escaping, validation, and
performance details remain library implementation details.

The coupling is intentional and temporary. It lets v0.3 ship and exercise the
JSON API without treating JSON's structure-and-field reflection needs as the
design for every decoration.

## Proposed restricted derived defaults

For post-v0.3 library-owned behaviors, a small general decoration contract can
target exactly one kind and derive complete default methods without member
reflection or application arguments. The syntax below is design material, not
accepted v0.3 source:

```flecha
export decoration cloneable {
    target structure;

    derive<T> {
        T clone(&const self) default {
            // Complete implementation.
        }
    }
};
```

`T` is bound to the decorated target. Within a derived method, `self` is the
runtime receiver of that target: `&const self` has type `&const T`. A static
derived method omits the receiver. The decoration itself has no instance and
no `self`.

The `default` modifier appears immediately before the method body. The method
is added unless the target provides an exact `override`. Signatures without
bodies are not default implementations.

Expansion is declaration-based rather than a textual preprocessing step:

1. resolve the decoration and bind `T`;
2. instantiate every derived method signature;
3. select exact user overrides and diagnose conflicts;
4. register the complete method set; and
5. type-check every derived and user body normally.

No capability is granted merely because a type is bound as `T`. Calling
`T::zero()` requires a zeroable target, copying `T` requires a copyable target,
and building, moving, and destroying it follow the ordinary ownership and
lifecycle rules. A failure is reported at the decoration application with
context pointing into the derived body.

## Post-v0.3 meta behavior direction

The intended 0.4 direction is to remove every JSON-specific compiler branch
while preserving the stable public declaration and application syntax. The
decoration identity and its implementation can be separated conceptually:

```flecha
export decoration json;

behavior json<T> {
    // Provisional syntax, not a v0.3 language contract.
    application(structure) target structure;
    application(string name) target field;

    derive {
        error writeJson(&const self, &Writer writer) default {
            // Restricted meta iteration over selected fields.
        }

        (own []u8, error) json(&const self) default {
            // Ordinary Flecha body calling self.writeJson(...).
        }

        error readJson(&self, &Reader reader) default {
            // Restricted meta dispatch over selected fields.
        }

        (T, error) fromJson([]const u8 source) default {
            // Ordinary Flecha body calling value.readJson(...).
        }
    }
};
```

The exact `behavior` and meta syntax remains to be designed. The required
capabilities are deliberately narrow:

- associate a behavior with a declared decoration;
- bind `T` to one target kind;
- match declared application forms and their literal argument types;
- inspect target fields, types, visibility, and attached decoration metadata
  read-only;
- iterate a selected field set deterministically;
- splice hygienic field accesses, names, and literals into ordinary methods;
- register all derived signatures before checking their bodies; and
- enforce exact overrides, expansion limits, and normal semantic rules.

The engine must not know the names `json`, `Writer`, `Reader`, `writeJson`, or
`readJson`. Those belong to Flecha source. It must also provide no filesystem,
environment, network, clock, process, FFI, arbitrary compiler-memory, or
unrestricted AST access. Generated methods pass through the ordinary semantic
pipeline and retain source context for useful diagnostics.

Once this restricted engine can express and test the complete JSON behavior,
the compiler-owned v0.3 implementation can be removed without changing
`export decoration json;` or any application source.

## Later interfaces and virtual requirements

The same default-method machinery may participate in a future interface
system. A decoration could declare interface conformance on behalf of its
target and satisfy members with derived defaults. A virtual method without a
default body would instead declare a requirement:

```flecha
(own []u8, error) foo() virtual = none;
```

Every decorated target would have to provide an exact override. A decoration
would not itself become a runtime interface; the interface system would define
the callable contract and dispatch model, while the decoration would declare
conformance and supply defaults.

When `default` and `virtual` become active syntax, editor integrations should
classify both as method-contract modifiers and render them in the same
dark-blue family used for virtual declarations, not the orange style used for
crashable operators. Neovim and Visual Studio Code highlighting should land
with the parser feature rather than reserving unsupported keywords early.
