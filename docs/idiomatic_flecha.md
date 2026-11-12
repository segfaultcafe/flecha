# Idiomatic Flecha Guideline

Status: living draft for Flecha 0.4

The Idiomatic Flecha Guideline, abbreviated **IFG**, defines the patterns that
make Flecha code recognizable, maintainable, and honest about ownership,
allocation, visibility, and dynamic behavior. These are the project's
"Padrões Flecha de Ser."

The standard library is both the primary laboratory and the canonical
reference for these patterns. A pattern used once in `std` is evidence, not an
automatic rule. It becomes idiomatic only after its tradeoffs are understood
and its status is recorded here.

This guideline complements the language specification. The specification says
what Flecha programs mean; the IFG says which valid designs Flecha libraries
should prefer.

## Normative language and maturity

The words **must**, **must not**, **should**, **should not**, and **may** are
normative. A rule can have one of these maturity levels:

- **Candidate**: a localized experiment in a real library or application;
- **Validated**: successful in at least two materially different contexts;
- **Idiomatic**: the recommended default for new Flecha code;
- **Stable**: suitable for broad migration and possible tooling enforcement;
- **Deprecated**: retained only for compatibility while code migrates.

Workarounds for compiler limitations do not become idiomatic merely because
they occur in `std`. They should be identified as compatibility patterns with
a condition for removal.

Every mature IFG rule should eventually record its rationale, canonical
example, counterexample, legitimate exceptions, language prerequisites, and
the standard-library modules that validate it.

## IFG 001 Abstract capabilities and preserve ownership

Status: Candidate; requires complete v0.4 interface dispatch

An API should accept an interface reference when it needs only a capability.
Ownership should remain explicit in a concrete type, owning adapter, or generic
parameter.

```flecha
interface Readable {
    (usize, error) read(&self, []u8 destination) virtual;
    error readExact(&self, []u8 destination) default;
};

interface Writable {
    (usize, error) write(&self, []const u8 source) virtual;
    error writeAll(&self, []const u8 source) default;
};

(usize, error) copy(
    &Readable source,
    &Writable destination,
    []u8 buffer,
);
```

Capability interfaces should:

- be minimal and orthogonal;
- keep primitive operations virtual;
- express algorithms derived from those primitives as `default` methods when
  that meaning belongs to every implementation;
- avoid slots added only for one implementation's convenience;
- make receiver mutability and borrow results exact; and
- avoid hidden allocation or ownership transfer.

Concrete values should normally be returned from constructors and factories.
An interface reference may be returned only when its data identity and borrow
origin remain valid under the declared contract. A structure should not store
a borrowed interface reference merely to erase a type when doing so obscures
the owner or extends the required lifetime.

Dynamic dispatch belongs at an abstraction boundary. Calls within a concrete
implementation should remain direct unless dynamic redispatch is part of the
contract.

Capability names should use adjectives when natural, such as `Readable` and
`Writable`. Concrete resources and owning adapters should use nouns such as
`File`, `Reader`, `Writer`, or `BufferedReader`.

The first validation target is `std::io`: keep the existing closure-owning
reader and writer structures as adapters, introduce capability interfaces,
make `copy` depend on those interfaces, and then let files, buffers, pipes, and
sockets conform directly where ownership permits.

## IFG 002 Separate physical organization from public namespaces

Status: Accepted direction; requires namespace attachment

A library's physical tree should organize implementation ownership and local
context. Its logical namespace should organize the API experienced by callers.
Those trees may differ deliberately through
[namespace attachment](namespace_attachment-v0.4.md).

A micro library with one focused context should prefer the direct layout:

```text
src/
├── reader.fl   -> module json
├── writer.fl   -> module json
└── value.fl    -> module json
```

A super library may use submodule folders without exposing each folder as a
public namespace:

```text
io/
├── stream.fl
└── stream/
    ├── reader.fl
    ├── writer.fl
    ├── copy.fl
    └── adapters/
        └── closure.fl
```

Source units in the submodule folders may contribute to `root::io::stream`:

```flecha
module stream;
under namespace root::io;
```

Libraries should follow these rules:

- use a submodule folder when a context has several implementation concerns
  that no longer scan well in one directory;
- keep a conventional anchor source at the expected direct path when it gives
  readers an obvious starting point;
- attach only modules whose logical home is genuinely the ancestor namespace;
- keep implementation-only parsers, platform layers, codecs, and helpers in
  their physical nested namespace when callers do not need them;
- do not use attachment only to shorten an import;
- let multiple attached source units compose one logical module when they
  represent one cohesive API; and
- use tooling and module indexes to expose every physical contributor to a
  logical module.

An import should reveal the conceptual dependency, not the current file-tree
location of its implementation.

## IFG 003 Name APIs by role

Status: Candidate consolidation of existing standard-library rules

Public names should communicate whether an operation is a free algorithm, a
receiver operation, a capability, or a concrete value.

- Modules, source files, and free functions use `snake_case`.
- Structures, enums, interfaces, error families, and type parameters use
  `PascalCase`.
- Members and member functions use `camelCase`.
- Acronyms behave as words: `isUtf8`, not `isUTF8`.
- Capability interfaces prefer role names such as `Readable`, `Writable`, or
  `Hashable` when that spelling is clear.
- Concrete and owning types prefer nouns such as `File`, `Socket`, `Reader`,
  and `Arena`.
- Boolean queries should read as predicates and avoid ambiguous negation.
- Names should not repeat a module name when qualification already supplies
  the context.

Existing public APIs remain subject to compatibility review. This rule guides
new design and deliberate migrations; it does not authorize opportunistic
renaming. Local-variable and parameter naming remains to be consolidated from
real standard-library use rather than fixed speculatively here.

## IFG 004 Choose free functions and members deliberately

Status: Candidate

Use a member when the operation is fundamentally about one receiver's state,
invariants, identity, or capability contract. Use a free function when the
operation is symmetric, combines several peer values, or is an algorithm whose
meaning does not belong to one receiver.

Members are preferred for:

- observing or mutating a receiver;
- maintaining a type invariant;
- constructors and lifecycle operations defined by the type;
- interface slots and their concrete implementations; and
- operations whose borrow result is naturally tied to the receiver.

Free functions are preferred for:

- algorithms over several equal participants;
- conversions where neither side clearly owns the operation;
- orchestration across multiple capabilities;
- package-level entry points; and
- operations that should work for types outside the defining module without
  expanding their member surface.

For example, `reader.read()` belongs to the receiver capability, while
`copy(&Readable, &Writable, buffer)` coordinates two peer capabilities and
belongs as a free function.

## IFG 005 Use the narrowest honest visibility

Status: Candidate consolidation of the logical-module model

Visibility is an architectural boundary, not a convenience for making code
compile.

- Leave a declaration unmarked when only its logical module needs it.
- Use `quiver` when multiple logical modules in one effective namespace need a
  shared contract or implementation detail.
- Use `export` only when callers outside the quiver are intended consumers of
  a supported API.
- Do not use `export` to avoid arranging modules or imports correctly.
- Do not use `quiver` for data that should remain an invariant of one type or
  one logical module.
- Public APIs must expose only nominal types whose own visibility supports that
  API.

Namespace attachment determines the effective namespace and therefore quiver
membership. A deeply stored attached source receives the same quiver view as a
direct source in that effective namespace, but it receives no additional
private access and inherits no imports.

Large sublibraries should keep most implementation declarations unmarked,
share deliberate cross-module machinery with `quiver`, and export a small
cohesive surface.

## Standard library adoption process

An IFG experiment in `std` should be introduced through a focused change with:

- the pattern and its intended boundary stated explicitly;
- tests that exercise the abstraction rather than only its implementation;
- allocation, ownership, borrow, and dispatch consequences documented;
- compatibility impact separated from stylistic cleanup;
- at least one counterexample showing where the pattern should not be used;
  and
- a follow-up decision to promote, revise, or remove the pattern.

The standard library should not be reformatted or reorganized wholesale merely
to resemble a draft rule. Each migration should improve one coherent subsystem
and provide evidence for the guideline.

## Guideline backlog

The following areas require dedicated rules as the standard library exercises
them:

- construction through `init`, inert `zero` values, and teardown;
- ownership in containers and adapters;
- checked-reference and slice return contracts;
- error families, partial results, and error preservation;
- generic constraints and type-set boundaries;
- interface composition, defaults, finals, and protected helpers;
- module facades, re-exports if introduced, and platform-specific modules;
- documentation of public contracts;
- test-module organization;
- unsafe and native boundaries;
- allocation visibility and performance promises;
- asynchronous APIs and atomic values; and
- decoration and behavior conventions when those features mature.

Existing detailed conventions remain authoritative where this draft has not
yet consolidated them, especially [library API style](library_api_style.md),
[standard-library development](std_development.md), and
[modules and imports](modules.md).
