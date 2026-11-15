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
primitive, owning `string`, owning slices (`own []T`), and recursively
decorated structures using the same Reader and Writer. Slice elements are
derived recursively, so nested owning slices and slices of decorated
structures are supported. Borrowed slices are outside the v0.3 derived-slice
contract; use `own []T`. A `stringv` field cannot be derived for reading
because decoded JSON text requires owned storage.

The following are compile-time errors:

- applying `@json(structure)` to anything other than a structure;
- applying `@json("name")` to anything other than a public structure field;
- using any other JSON application argument shape;
- selecting the same serialized name more than once;
- selecting a field without a supported JSON representation; and
- using a borrowed slice, or using `stringv` with a Reader-backed decoration.

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

## Typed declarations (LANG-01)

The compiler also accepts declaration-driven application contracts:

```flecha
decoration wire {
    application(structure) target structure;
    application(stringv name) target structure::field;
    application(stringv name) target enum;
    application(stringv name) target interface;
};
```

A body declares at most one signature per target category. `application` and
`target` are contextual words, so ordinary functions and variables can still
use those names. `structure` is the initial literal marker. Named parameters
use the existing module-constant value types: numeric primitives, `bool`,
`char`, `stringv`, enums, and error values. Arguments use ordinary compile-time
expressions and module constants; owning values and runtime expressions are
rejected. An empty body declares no permitted targets. The legacy declaration
without a body retains its compiler-owned validation.

Imports, source-local aliases, logical modules, namespace attachments, and
quiver visibility apply to decoration identities. Duplicate direct
applications are rejected independently for each declaration or field.
Interface metadata is direct and does not propagate through conformance.

Typed declarations only validate metadata, including when named `json`. They
do not invoke the compiler-owned JSON path, create methods, add conformance,
or emit runtime metadata. Existing `export decoration json;` declarations and
JSON derivation remain unchanged. LANG-02 registers behavior contracts and
activation candidates as described below. Bounded reflection plans feed
transactional provider selection, concrete conformance, ordinary method
analysis, and lowering. The JSON library migration remains deferred.

Contract diagnostics use stable identifiers `FLECHA-S0066` through
`FLECHA-S0074`, alongside existing parse, unresolved-name/type, and duplicate
declaration diagnostics. Contract errors include related declaration locations
in CLI output and analyzer/LSP diagnostics.

## Accepted v0.4 replacement

The accepted v0.4 design separates decoration metadata, callable contracts,
and derivation. A decoration declares typed application forms, an interface
declares receiver methods, and a behavior derives conformance plus
implementations from the decorated structure's fields.

```flecha
export decoration json {
    application(structure) target structure;
    application(stringv name) target structure::field;
};

behavior json<T> => structure << JsonWritable, JsonReadable {
    error T::writeJson(&const self, &Writer writer) override {
        for<F> Field<T, F> field = T::fields() {
            // Ordinary, statically typed Flecha.
        }
    }

    error T::readJson(&self, &Reader reader) override {
        for<F> Field<T, F> field = T::fields() {
            // Ordinary, statically typed Flecha.
        }
    }
};
```

`T` is the decorated structure and `F` is the concrete type of one field-loop
expansion. Behaviors do not introduce a dynamically typed meta sublanguage.
They may implement only receiver methods declared by interfaces in their `<<`
list. Static factories such as the v0.3 `Response::fromJson` do not become
static interface requirements; the v0.4 form is an ordinary constrained
generic helper such as `json::from<Response>(source)`.

Behaviors are limited to structure and enum targets. Decorations may also
target interfaces, but those applications are non-propagating metadata and do
not activate behavior.

The complete contract, including activation, field access, handwritten
override precedence, allowed operations, allocation, and diagnostics, is in
[behaviors-v0.4.md](behaviors-v0.4.md). Its callable and dispatch rules come
from [interfaces-v0.4.md](interfaces-v0.4.md), rather than defining a second
interface model inside the behavior engine. The decision record and required
completion criteria are [IFG 2026-0006](IFG/IFG_2026_0006.txt).

Once the restricted behavior system can express and test JSON, the
compiler-owned v0.3 implementation can be removed. The existing decoration
applications remain expressible, while v0.4 may replace generated convenience
methods with ordinary generic library functions.

When `virtual`, `default`, and `final` become active syntax, editor integrations
should classify them as method-contract modifiers and render them in the same
dark-blue family, not the orange style used for crashable operators. Neovim and
Visual Studio Code highlighting should land with the parser feature rather
than reserving unsupported keywords early.

## Behavior declaration semantics (LANG-02)

A typed decoration can own one behavior for structures and one for enums in
its logical module. A declaration accepts one unconstrained target binder:

```flecha
behavior meta<T> => structure << Writable {
    i32 T::write(&const self, i32 value) override {
        return value;
    }
};
```

The compiler records the owner, interface roots, and checked signatures in a
`BehaviorContract`. Matching structure applications and direct-field
applications are collected into one `BehaviorActivationCandidate` per behavior
and concrete owner. Enum applications activate only enum behaviors;
interface-target metadata never activates one. Imported applications can
activate an owning module's behavior, but another module cannot declare it.
Files attached to the same logical namespace/module share ownership.

Declared methods must exactly match reachable interface requirements, including
receiver, parameters, result, visibility, effects, variadics, borrowing and
ownership. They must use `override`; unrelated methods and final replacement
are errors. Duplicate roots, related roots, shared ancestors and colliding
method keys are rejected, including the prospective union with explicit
conformance and other active behaviors. Repeated field applications activate
a behavior once and preserve all field metadata.

Behavior registration produces semantic records before expansion. Active
behaviors must satisfy every virtual requirement after exact handwritten
providers and inherited defaults/finals are selected. An inactive behavior may
omit implementations; an activated one may not leave a virtual unresolved.
`T::fields()`, `Field<T,F>`, and `for<F>` feed the bounded plans below. The
compiler-owned JSON path and runtime representation remain unchanged.

Behavior diagnostics use `FLECHA-S0075` (invalid declaration/owner/root),
`FLECHA-S0076` (target parameter), `FLECHA-S0077` (duplicate behavior),
`FLECHA-S0078` (method contract), and `FLECHA-S0079` (conflicting roots).

## Bounded reflection plans (LANG-03)

Typed metadata is evaluated in the module-constant scalar domain and exposed
through a `TypedDecorationRecord`. Bounded field iteration and target metadata
queries produce detached `BehaviorExpansionPlan` records. Field descriptors
retain declaration identity, exact closed field types and ownership, and source
order. Provider selection precedes expansion: an exact handwritten `override`
suppresses its behavior body completely. All surviving signatures are staged
before checking any body. Expanded functions then use ordinary semantic, borrow,
allocation, generic, and lowering machinery. Only successful transactions expose
concrete methods and conformance; failures leave the concrete graph unchanged.

Reflection diagnostics use `FLECHA-S0080` (iteration shape), `FLECHA-S0081`
(context/control flow), `FLECHA-S0082` (operation or receiver), `FLECHA-S0083`
(compile-time artifact escape), `FLECHA-S0084` (missing decoration proof),
`FLECHA-S0085` (metadata member), and `FLECHA-S0086` (evaluation/expansion).
Ordinary type, ownership, and borrow diagnostics retain their identifiers and
include related behavior, activation, target, and field locations.

Behavior-derived interface calls use the existing composed-table ABI. No field
pack, decoration record, or reflection operation reaches runtime. Generated
method navigation and diagnostics retain their original source provenance.
`break`, `proceed`, and `jumpto` inside compile-time field loops remain rejected.

See [behaviors-v0.4.md](behaviors-v0.4.md) for provider selection, publication,
and the separately deferred JSON migration.
