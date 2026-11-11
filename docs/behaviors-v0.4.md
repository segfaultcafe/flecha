# Behaviors and decorations for Flecha 0.4

Status: proposed language design; not implemented

Behaviors let a library derive an interface implementation for a structure or
enum from typed decoration metadata. They are deliberately narrower than
macros or general compile-time execution. A behavior may add declared
interface conformance and provide receiver-method bodies for that contract; it
may not invent an unrelated public API or rewrite the decorated declaration.

The model has three separate responsibilities:

- a `decoration` declares the applications and typed metadata accepted in
  source;
- an `interface` declares the callable contract visible to ordinary Flecha
  code; and
- a `behavior` derives conformance and implementations for a decorated
  structure or enum.

Behavior bodies are Flecha bodies. The only special iteration facility
currently specified is bounded, typed iteration over the direct fields of a
structure target. There is no dynamically typed meta language and no general
`meta` execution mode.

## Decoration declarations

A behavior-capable decoration declares every accepted application shape. The
JSON decoration needs a structure marker and a field-name override:

```flecha
export decoration json {
    application(structure) target structure;
    application(stringv name) target field;
};
```

The declarations accept the existing applications:

```flecha
@json(structure)
structure Response {
    i32 value;
    @json("display-name") string name;
};
```

`structure` in the first application is the literal marker used by
`@json(structure)`. `stringv name` in the second form is a named, typed
compile-time argument. An application argument must be a compile-time value of
its declared Flecha type. Its value is metadata; declaring or applying the
decoration does not execute code.

Target validation is declaration-driven. The compiler rejects the wrong
argument count or type, an unsupported target kind, duplicate applications
where the decoration does not permit them, and inaccessible decoration names
before expanding a behavior.

The initial contract permits at most one application signature for each
decoration target kind. This keeps typed metadata lookup unambiguous. Tagged
application alternatives can be designed later if a concrete use requires
them.

Decorations may also target an interface:

```flecha
export decoration protocol {
    application(stringv name) target interface;
};

@protocol("transport")
interface Transport {
    error send(&self) virtual;
};
```

An interface-target application is typed metadata only. It cannot activate a
behavior, add or replace methods, change interface composition, or propagate
to descendant interfaces or conforming concrete types.

## Interfaces own the generated API

A behavior does not list arbitrary methods to attach to a target. The library
first declares ordinary interfaces using the
[v0.4 interface model](interfaces-v0.4.md):

```flecha
export interface JsonWritable {
    error writeJson(&const self, &Writer writer) virtual;
    (own []u8, error) json(&const self) default;
};

export interface JsonReadable {
    error readJson(&self, &Reader reader) virtual;
};
```

The convenience writer remains an ordinary receiver default:

```flecha
(own []u8, error) JsonWritable::json(&const self) {
    Writer writer = Writer::init();

    error err = self->writeJson(&writer);
    if err != none {
        return (none, err);
    }

    return writer.finish();
}
```

This body is normal Flecha and its call to `writeJson` dispatches according to
the interface rules. It is not expanded once per field and has no special
access to decoration metadata.

Interfaces continue to contain receiver signatures only. Construction is a
type-level operation and is expressed by an ordinary generic function rather
than a static interface requirement:

```flecha
export (T, error) from<T: JsonReadable>([]const u8 source) {
    T value = T::zero();
    Reader reader = Reader::init(source);

    error err = value.readJson(&reader);
    if err != none {
        return (move value, err);
    }

    err = reader.finish();
    return (move value, err);
}
```

A caller names both the operation's module and the constructed type:

```flecha
(Response response, error err) = json::from<Response>(source);
```

The generic function is checked through normal specialization. If its body
requires `T::zero()`, the supplied type must support that operation. There is
no `Self` placeholder, static virtual dispatch, or call through an erased
interface reference.

## Behavior declaration

The JSON behavior names its decoration, binds `T` to the affected structure,
and lists the interfaces it derives:

```flecha
behavior json<T> << JsonWritable, JsonReadable {
    error T::writeJson(&const self, &Writer writer) override {
        for<F> Field<T, F> field = fields<T> {
            // Ordinary Flecha using this typed field projection.
        }
    }

    error T::readJson(&self, &Reader reader) override {
        for<F> Field<T, F> field = fields<T> {
            // Ordinary Flecha using this typed field projection.
        }
    }
};
```

For a decorated `Response`, `T` is `Response`. In one field-loop expansion,
`F` is the exact type of the current field. Given:

```flecha
structure Response {
    string name;
    i32 status;
    bool cached;
};
```

the compiler analyzes the loop body as if it had three typed expansions:

```flecha
Field<Response, string>
Field<Response, i32>
Field<Response, bool>
```

`T` remains `Response`; `F` changes for each field. Both are ordinary generic
type parameters, not runtime type objects or dynamically typed meta values.

The `<<` list is the complete public capability a behavior may add. Every
method declared in the behavior must exactly implement or override a receiver
method reachable from one of those interfaces. It must use `override`, and it
is subject to the same signature, visibility, `default`, `final`, borrow,
ownership, and effect rules as a handwritten structure implementation.

A behavior may omit an interface default and inherit it normally. It cannot
replace a final method. It also cannot define a target method that is absent
from its declared interface graph.

## Behavior targets, activation, and scope

Behaviors may target structures and enums only. Applying a suitable decoration
directly to either concrete type may activate its behavior. An application
whose declared target is `interface` never does.

For a structure behavior, applying the decoration to a direct field associates
that metadata with the field's owning structure and may also activate the
behavior. This preserves explicit JSON selection such as:

```flecha
structure ExplicitResponse {
    i32 ignored;
    @json("value") i32 value;
};
```

Here `T` is `ExplicitResponse` even though the structure has no
`@json(structure)` marker. The behavior decides, in Flecha, whether a
structure-level application selects all eligible fields or field-level
applications select only their fields.

Only applications written on the structure declaration or its direct fields
belong to that structure behavior expansion. Nested structures are separate
targets and must establish their own conformance. An enum behavior is activated
by an application to the enum declaration itself. It may add its declared
interface conformance and receiver-method implementations, but receives no
variant or discriminant reflection. Variant-target activation and variant
generation are also unavailable.

A behavior must be declared in the same logical module as its decoration. A
decoration has at most one behavior for each supported concrete target kind.
Importing another module cannot add or replace behavior for a decoration it
does not own. This makes conformance coherent and prevents imports from
retroactively changing a type's API. If one decoration supports both structure
and enum behavior, its declarations must distinguish which target kind binds
`T`; the exact surface spelling for that distinction remains to be selected.

## Typed field iteration

For a structure behavior, `fields<T>` is a compile-time field pack, not a
runtime collection.

```flecha
for<F> Field<T, F> field = fields<T> {
    // body
}
```

The compiler expands it in source declaration order and then type-checks each
specialized body normally. An empty structure produces zero expansions. The
loop cannot use runtime `break` or `continue`, and neither `Field<T, F>` nor a
field pack has a runtime representation.

A field descriptor provides only bounded, typed information:

- its owning type is `T`;
- its field type is `F`;
- its declaration name is a compile-time `stringv`;
- its visibility and decoration presence are compile-time values;
- arguments for a present decoration retain their declared Flecha types; and
- `field.get(receiver)` is a hygienic projection of that exact field.

The projection behaves like writing the concrete member access in source. A
mutable receiver produces an assignable place; a const receiver produces a
const place. Copying, moving, borrowing, assigning, or passing the result is
then governed by the ordinary rules for `F`. Merely projecting a field does
not copy or allocate it.

The exact surface spelling for retrieving a present decoration's typed
argument record is still open. It must not return an untyped map, `any` value,
raw syntax tree, or stringly typed argument. Whichever spelling is selected,
the field form above must expose JSON's `name` as a `stringv` known during that
field expansion.

Field iteration is available only inside structure-behavior implementations
for their bound target. It does not provide module enumeration, base-address
arithmetic, offset mutation, arbitrary member lookup by string, or reflection
over unrelated types. A structure behavior body receives target-method access
to the target's private fields because applying the decoration is an explicit
opt-in by the type author. The behavior may still inspect visibility and
deliberately apply a public-only policy such as JSON's v0.3 contract.

## Explicit implementations and conflicts

An exact handwritten target implementation takes precedence over the matching
behavior implementation. The compiler does not instantiate the derived body
for that slot:

```flecha
@json(structure)
structure Response {
    i32 value;
};

error Response::writeJson(&const self, &Writer writer) override {
    // Application-specific implementation.
    return none;
}
```

The handwritten method must still be a valid `override` of the derived
interface contract. A same-named method with a different signature is an
overload and does not suppress behavior expansion.

If several active behaviors add the same canonical interface requirement, the
normal interface merge rules apply. One handwritten exact implementation may
satisfy all matching requirements. Two behavior-provided bodies for the same
slot are ambiguous and rejected; source order and decoration order never pick
a winner. Final-method and unrelated-default conflicts remain interface
errors, not behavior precedence rules.

## Allowed operations

After field-pack expansion, a behavior implementation is ordinary Flecha. It
may:

- add conformance only to the interfaces in its `<<` list;
- implement virtual methods and override defaults from those interfaces;
- use locals, calls, control flow, overloads, generics, errors, and qualified
  interface calls;
- inspect the direct fields and typed decoration metadata of `T` when `T` is a
  structure;
- project structure fields through `Field<T, F>` under normal access, borrow,
  and ownership checking; and
- call ordinary APIs that allocate, perform I/O, or have other effects when
  their signatures and the enclosing method permit those operations.

It may not:

- add, remove, reorder, or change fields or otherwise alter target layout;
- activate a behavior from an interface-target decoration or propagate that
  decoration through interface conformance;
- enumerate, inspect, add, remove, or rewrite enum variants or discriminants;
- invent methods outside its declared interface graph;
- introduce types, interfaces, decorations, imports, or module members during
  expansion;
- change visibility, lifecycle declarations, ownership contracts, or an
  existing method body;
- replace a final method or bypass an unresolved virtual requirement;
- generate identifiers or source from strings, inject raw tokens, or access an
  unrestricted syntax tree;
- enumerate unrelated declarations, modules, files, processes, environment
  variables, the network, the clock, or compiler memory;
- bypass type checking, definite assignment, borrowing, allocation checking,
  unsafe boundaries, or specialization budgets; or
- leave runtime reflection data behind unless an ordinary declared API
  explicitly requires such data.

This boundary allows serialization, cloning, hashing, equality, visitors, and
similar field-derived contracts without making every library package a
compiler plugin.

## Allocation and runtime representation

Decoration metadata, field packs, and field descriptors exist only while the
compiler expands and checks a behavior. They emit no runtime objects.
Behavior-derived conformance uses the same static interface tables and
two-word checked interface references as handwritten conformance. It does not
add a hidden pointer to a structure or change an enum's backing representation,
and it does not allocate.

A behavior method can still call ordinary allocating code. JSON decoding may
allocate owned strings and slices, and `json()` may allocate its result buffer.
Those allocations come from the called Flecha APIs and retain their normal
error and ownership contracts; they are not implicit costs of decorations,
behaviors, field iteration, or interface conversion.

## Expansion and diagnostics

The compiler processes a behavior application in this order:

1. resolve and type-check every decoration application;
2. identify the concrete structure or enum target and bind `T`;
3. add the behavior's declared interface conformance edges;
4. instantiate and register all candidate receiver signatures;
5. select exact handwritten implementations and diagnose body conflicts;
6. expand bounded field packs, binding a concrete `F` for each field;
7. validate the complete interface graph and remaining virtual requirements;
8. type-check every surviving body through the normal semantic pipeline; and
9. lower only the resulting ordinary Flecha declarations.

An error in a specialized field body is reported at the decoration application
with related context for the behavior source, field declaration, concrete `T`
and `F`, and failed operation. Expansion obeys the existing generic depth,
declaration, and token budgets. Exceeding a budget is a compile-time error.

## Deliberately deferred

The initial design does not include:

- static interface requirements or static behavior methods;
- behavior-defined free functions or arbitrary associated methods;
- behaviors targeting interfaces, fields independently of their owning
  structure, free functions, parameters, statements, or expressions;
- enum-variant activation, generation, reflection, or a compile-time variant
  pack;
- adding fields or lifecycle hooks;
- variadic or tagged decoration application alternatives;
- runtime reflection or dynamic decoration queries;
- source-generating macros and unrestricted compile-time functions; or
- a stable cross-compiler behavior-expansion ABI.

Type-level helpers such as `json::from<T>` remain ordinary library functions.
If Flecha later gains extension functions or type-associated free functions,
they can improve call-site spelling independently of the interface and
behavior contracts.
