# Interfaces for Flecha 0.4

Status: accepted language contract; not implemented

Interfaces define nominal receiver-method contracts without adding storage,
implementation inheritance, or hidden allocation. Structures and enums conform
to interfaces with `<<`; interfaces compose other interfaces with the same
syntax. Interface values do not exist. Runtime polymorphism is exposed through
checked interface references backed by immutable, statically emitted dispatch
tables.

This document fixes the v0.4 source contract, composition rules, method
identity, vertical method-state transitions, reference casts, and dispatch-table
layout. A stable cross-compiler binary ABI remains outside the v0.4 guarantee.

## Declaration model

An interface contains receiver-method signatures only. It cannot contain
fields, constants, storage, lifecycle declarations, static methods, or nested
types.

```flecha
export interface Service {
    error start(&self) virtual;
    error status(&const self) default;
    error stop(&self) final;

    stringv name(&const self) virtual <-self;
    protected error validate(&const self) default;
};
```

Every interface method declares exactly one category:

- `virtual` has no body and remains a requirement;
- `default` has an out-of-line body and may be overridden; and
- `final` has an out-of-line body and may not be overridden.

A `virtual` method must not have a body. Every `default` and `final` method must
have exactly one out-of-line body. Bodies never appear inline in an interface
declaration.

For a method originated by the interface, the body does not use `override`:

```flecha
error Service::status(&const self) {
    return none;
}

error Service::stop(&self) {
    return none;
}

protected error Service::validate(&const self) {
    return none;
}
```

The category belongs to the interface signature and is not repeated on its
out-of-line body. `override` belongs to an out-of-line body that replaces an
inherited slot, as specified below.

The initial interface contract accepts only checked-reference receivers:

```flecha
&self
&const self
```

Static methods, owning receivers, raw-pointer receivers, fields, constants,
`init`, `zero`, and `tear` do not participate in interface conformance.

### No interface generics in v0.4

Interface declarations and interface methods cannot introduce generic
parameters in v0.4:

```flecha
interface Serializer<T> {               // Error.
};

interface Writer {
    error write<T>(&self, T value) virtual; // Error.
};
```

An unbounded generic virtual method cannot have a finite dispatch-table shape.
Ordinary generic functions may instead constrain their type parameters by an
interface:

```flecha
error encode<T: JsonWritable>(&Writer writer, &const T value) {
    return value->writeJson(writer);
}
```

Generic structures and enums may conform to non-generic interfaces; this is
specified under [Generic concrete types](#generic-concrete-types).

## Method identity and exact contracts

The compiler separates overload identity from the complete method contract.

```text
MethodKey =
    method name
    + normalized receiver
    + explicit parameter shapes
    + variadic shape

MethodContract =
    MethodKey
    + result type and ownership
    + parameter ownership contracts
    + borrow-result sources
    + existing call effects
```

The receiver is implicit parameter zero. Its concrete owner is normalized to
`Self`, so an interface declaration and a concrete implementation can match
without sharing an owner type. Explicit parameters are numbered from one.
Parameter names do not participate in identity.

`&self` and `&const self` produce different `MethodKey`s and therefore may be
overloaded:

```flecha
interface Value {
    stringv get(&self) virtual <-self;
    stringv get(&const self) virtual <-self;
};
```

For a mutable receiver, the `&self` overload is the exact match and ranks ahead
of conversion to `&const self`. A const receiver can select only the
`&const self` overload.

Return types, result ownership, borrow sources, visibility, and call effects do
not create overloads. If two declarations have the same `MethodKey` but differ
in one of those properties, they conflict. Methods cannot be overloaded only
by return type.

Call-effect matching retains Flecha's existing function compatibility rules.
Interfaces introduce no new effect variance. Default arguments, where
otherwise permitted by the language, do not change a parameter signature.

### Borrow-result normalization

Borrow sources are part of the complete contract. Source names normalize to
parameter positions:

```text
self              -> fixed parameter 0
first explicit    -> fixed parameter 1
second explicit   -> fixed parameter 2
...
variadic pack     -> variadic-pack sentinel
```

Renaming a parameter therefore does not change the contract. Multiple borrow
sources are an order-independent, deduplicated set. These contracts are
equivalent:

```flecha
<-(self, fallback)
<-(fallback, self)
```

A named variadic pack is a valid borrow source:

```flecha
interface Reader {
    stringv read(&const self, values...) virtual <-...values;
};
```

`...values` denotes the pack as one canonical source. At a call it covers all
actual arguments supplied through that pack; an empty pack contributes no
dynamic origin. A borrow contract may combine fixed and variadic sources:

```flecha
stringv select(&const self, stringv fallback, values...)
    virtual <-(self, fallback, ...values);
```

An override must declare the same normalized borrow-source set. Borrow
contracts are exact in v0.4; an override cannot weaken, strengthen, or reorder
the semantic set of sources.

### Modifier and borrow order

On an interface signature, the borrow contract follows the method category:

```text
(parameters) (virtual | default | final) [borrow-contract] ;
```

On an overriding body, it follows `override`:

```text
(parameters) override [borrow-contract] body
```

On a body that originates a method and therefore has no category or
`override`, it follows the parameter list directly:

```flecha
interface Named {
    stringv rawName(&const self) virtual <-self;
    stringv name(&const self) default <-self;
};

stringv Named::name(&const self) <-self {
    return self->rawName();
}
```

The canonical v0.4 spelling is therefore:

```flecha
protected stringv Entity::name(&const self) override <-self {
    return self->name;
}
```

The legacy order `... <-self override` may be accepted temporarily with a
migration diagnostic, but is not canonical v0.4 syntax.

## Visibility

Interface methods are public unless declared `protected`. A protected method is
accessible from:

- method bodies belonging to the declaring interface;
- method bodies belonging to descendant interfaces; and
- methods belonging to structures or enums that conform to the interface,
  directly or transitively.

Unrelated callers cannot name a protected method. Every declaration and
out-of-line implementation that explicitly continues a protected method must
repeat `protected`:

```flecha
interface Validated {
    protected error validate(&const self) virtual;
};

structure Record << Validated {
};

protected error Record::validate(&const self) override {
    return none;
}
```

Omitting `protected`, or replacing it with another visibility, is an exact
contract mismatch. The absence of `protected` is the public spelling. A public
method cannot be narrowed to protected, and a protected method cannot be
widened silently.

A structure or enum may use `protected` only when continuing a protected
interface slot. It cannot originate an unrelated protected concrete method:

```flecha
protected error Record::helper(&const self) { // Error.
    return none;
}
```

Concrete helpers use the existing `private` rule instead. `private` interface
methods are not part of v0.4.

Visibility does not create overloads and is not part of `MethodKey`; it is
validated separately as an exact property of one method lineage.

## Nominal conformance

A structure declares nominal conformance after its name:

```flecha
export structure Worker << Service {
    i32 identifier;
};
```

An enum uses the same operator. With an explicit backing type, the conformance
list follows that type:

```flecha
export enum State: u8 << Displayable {
    Off,
    On,
}
```

Without a backing type, `<<` follows the enum name. Interface declarations also
use `<<` to compose parent interfaces.

`<<` may appear only once in a declaration header. Its right-hand side is a
comma-separated list of direct interfaces. Chaining is invalid:

```flecha
structure Gee << Bar << Foo { // Error.
};
```

Conformance adds no base subobject, fields, constructor or destructor ordering,
lifecycle hooks, or hidden table pointer to the concrete value. Enum
conformance does not change its backing representation.

## Safe composition

For a declaration:

```flecha
D << A1, A2, ..., AN
```

the direct parent list must be an antichain, its ancestry closures must be
disjoint, and its effective method sets must be disjoint. For every `j != k`:

```text
Aj != Ak

not reachable(Aj, Ak)
not reachable(Ak, Aj)

ancestors(Aj) intersect ancestors(Ak) is empty

MethodKeys(Aj) intersect MethodKeys(Ak) is empty
```

Equivalently, the reflexive ancestry closures of two direct parents may not
intersect. The checks remain conceptually separate so diagnostics can identify
the most useful cause.

### No parent may implement another parent

Even an empty redundant edge is invalid:

```flecha
interface A {
};

interface B << A {
};

structure D << A, B { // Error: B already conforms to A.
};
```

The correct declaration is `D << B`.

### No diamond paths

One ancestor may not reach a declaration through multiple direct parents:

```flecha
interface Root {
    error close(&self) virtual;
};

interface Left << Root {
};

interface Right << Root {
};

interface Combined << Left, Right { // Error: diamond through Root.
};
```

The direct diamond `Combined << Root, Left` is also invalid because one parent
implements another.

### No horizontal method merging

Independent parents may not contribute the same `MethodKey`, even when their
complete contracts and categories are identical:

```flecha
interface A {
    error run(&self) virtual;
};

interface B {
    error run(&self) virtual;
};

structure C << A, B { // Error: duplicate run(&self) slot.
};
```

A method written on `C` cannot resolve this conflict. Composition is validated
before implementations are selected. The same rejection applies to every
horizontal category pairing: virtual/virtual, virtual/default,
virtual/final, default/default, default/final, and final/final.

Methods with different `MethodKey`s remain ordinary overloads and may come
from independent parents:

```flecha
interface IntegerWriter {
    error write(&self, i32 value) virtual;
};

interface StringWriter {
    error write(&self, stringv value) virtual;
};

structure Writer << IntegerWriter, StringWriter { // Valid.
};
```

These rules apply equally when the conforming declaration is an interface,
structure, enum, or a concrete type receiving conformance from a behavior.

## Vertical method lineages

An exact declaration in a descendant interface continues an inherited slot
instead of creating a new one:

```flecha
interface Foo {
    error foo(&self) virtual;
};

interface Bar << Foo {
    error foo(&self) virtual;
};

structure Gee << Bar {
};

error Gee::foo(&self) override {
    return none;
}
```

The conceptual state is:

```text
canonical origin:      Foo::foo
effective declaration: Bar::foo
effective category:    virtual
concrete provider:     Gee::foo
```

`Bar::foo` does not add a second slot. `Gee::foo` satisfies the complete
lineage and is used through both `&Bar` and `&Foo`.

A vertical redeclaration must preserve the full contract, including
visibility, result, ownership, borrow sources, and effects. A changed return or
borrow contract conflicts with the inherited slot. A changed receiver or
parameter shape creates a different overload and leaves the inherited
requirement in place.

### Category state machine

A method lineage may originate in any category. Its state can only advance:

```text
virtual -> virtual
virtual -> default
virtual -> final

default -> default
default -> final

final   -> no redeclaration
```

`final` is optional but terminal. A lineage may remain virtual until a concrete
type implements it, may end at any default, or may be sealed by a final. A
default cannot be made virtual again.

| Inherited state | Descendant declaration | Result |
| --- | --- | --- |
| `virtual` | omitted | remains virtual |
| `virtual` | `virtual` | remains virtual |
| `virtual` | `default` | gains an overridable body |
| `virtual` | `final` | gains a body and is sealed |
| `default` | omitted | inherits the current default |
| `default` | `default` | replaces the current default |
| `default` | `final` | replaces the body and seals the lineage |
| `default` | `virtual` | error: a provided lineage cannot be re-abstracted |
| `final` | omitted | inherits the final body |
| `final` | any redeclaration | error: the lineage is sealed |

Example:

```flecha
interface A {
    error run(&self) virtual;
};

interface B << A {
    error run(&self) virtual;
};

interface C << B {
    error run(&self) default;
};

error C::run(&self) override {
    return none;
}

interface D << C {
    error run(&self) default;
};

error D::run(&self) override {
    return C::run(self);
}

interface E << D {
    error run(&self) final;
};

error E::run(&self) override {
    return D::run(self);
}
```

A direct `virtual -> final` transition is valid:

```flecha
interface Requirement {
    error commit(&self) virtual;
};

interface Sealed << Requirement {
    error commit(&self) final;
};

error Sealed::commit(&self) override {
    return none;
}
```

### Concrete obligations

The effective category determines the concrete type's obligation:

| Effective category | Body | Concrete type obligation |
| --- | --- | --- |
| `virtual` | forbidden on the interface declaration | must override |
| `default` | required | may inherit or override |
| `final` | required | must inherit; cannot override |

An exact concrete method definition is an error for a final slot whether or
not it is marked `override`. A differently shaped overload remains permitted.

## Placement of `override`

`override` belongs to an out-of-line implementation, never to an interface
signature declaration. Whether it is required depends on whether the body
creates a slot or replaces an inherited slot.

An originating default or final body does not use `override`:

```flecha
interface Local {
    error inspect(&self) default;
};

error Local::inspect(&self) {
    return none;
}
```

A descendant interface body that provides or replaces an inherited slot must
use `override`:

```flecha
interface Parent {
    error inspect(&self) virtual;
};

interface Child << Parent {
    error inspect(&self) default;
};

error Child::inspect(&self) override {
    return none;
}
```

The same rule applies to `virtual -> final`, `default -> default`, and
`default -> final` bodies. A concrete implementation of an effective virtual or
default method also requires `override`:

```flecha
structure Concrete << Child {
};

error Concrete::inspect(&self) override {
    return none;
}
```

Replacing an inherited final method is always invalid.

## Generic concrete types

A generic structure or enum may conform to a non-generic interface:

```flecha
interface Inspectable {
    error inspect(&const self) virtual;
};

structure Box<T> << Inspectable {
    T value;
};

error Box<T>::inspect(&const self) override {
    return none;
}
```

The conformance applies to every valid closed specialization of `Box<T>`. The
open type itself has no runtime interface value or dispatch table. Each closed
specialization used as an interface receives its own static table.

An implementation of a non-generic interface slot cannot introduce method
generics. The concrete type may still define unrelated generic overloads:

```flecha
U Box<T>::convert<U>(&const self); // Valid, but not an override.
```

If an implementation body requires a capability from `T`, the owner must
declare that constraint rather than inferring conditional conformance from the
body:

```flecha
structure DisplayBox<T: Inspectable> << Inspectable {
    T value;
};

error DisplayBox<T>::inspect(&const self) override {
    return self->value.inspect();
}
```

Normal specialization checks the body for every instantiated `T`. Flecha does
not infer a conformance edge only for the specializations whose bodies happen
to compile.

## Interfaces are reference-only

An interface has no value representation and cannot be instantiated, copied,
moved, built, or stored by value, including as a structure field. Runtime
polymorphism uses checked interface references:

```flecha
&Interface
&const Interface
```

An interface reference retains the original concrete borrow origin. It cannot
outlive the concrete value. A mutable interface reference excludes the same
conflicting accesses, moves, replacement, destruction, and other mutable loans
as an ordinary checked reference. Returned interface references use the normal
borrow-source contract.

## Explicit interface casts

The existing `as` cast has a built-in interface-reference form. Casting a
conforming lvalue both creates a checked loan and selects the requested
interface view:

```flecha
Gee g;

gFoo := g as &Foo;
```

The target may likewise be `&Bar` or `&Baz`. Each mutable cast is valid when no
conflicting loan of `g` is live; the ordinary borrow rules still apply.

The explicit declaration form is equivalent:

```flecha
&Foo gFoo = g as &Foo;
```

The cast target must be `&Interface` or `&const Interface`; an interface cannot
be the target of a value or owning cast. The source concrete type must conform
to the target directly or transitively. Failure to prove conformance is a
compile-time error; this cast never performs a runtime type test and cannot
fail dynamically.

The cast preserves ordinary mutability rules:

```flecha
Gee g;

mutableFoo := g as &Foo;
constFoo := g as &const Foo;
```

A const lvalue can produce only `&const Interface`. Several mutable interface
views of one value cannot coexist. Several const views may:

```flecha
foo := g as &const Foo;
bar := g as &const Bar;
baz := g as &const Baz;
```

The cast does not extend temporary lifetime:

```flecha
foo := Gee::init() as &Foo; // Error: the loan would escape the temporary.
```

The same cast works from a checked concrete reference and for a child-interface
to parent-interface upcast:

```flecha
&Gee concrete = &g;
bar := concrete as &Bar;
foo := bar as &Foo;
```

Downcasts and sideways casts are invalid:

```flecha
foo := g as &Foo;

bar := foo as &Bar; // Error: downcast.
baz := foo as &Baz; // Error: sideways cast.
```

The hidden concrete value is not dynamically inspected. Runtime downcasting,
sideways conversion, and fallible interface casts are deferred.

The cast is also available during specialization when constraints prove the
path:

```flecha
error operate<T: Bar>(&T value) {
    foo := value as &Foo;
    return foo->foo();
}
```

### Contextual reference conversions

Existing checked references may still convert implicitly in an unambiguous
expected-type context:

- `&Concrete` to an implemented `&Interface`;
- `&Concrete` to a transitively implemented parent interface;
- `&ChildInterface` to `&ParentInterface`; and
- the corresponding mutable-to-const forms.

The conversion is allocation-free and preserves the existing borrow origin.
Concrete values never acquire an implicit loan; use `value as &Interface` or an
explicit concrete reference. An exact concrete-reference overload ranks ahead
of an interface conversion.

If a concrete type implements two unrelated interface parameter types and a
call could select either, overload resolution is ambiguous. `as &Interface`
selects the intended view explicitly.

## Static conformance queries

`isof` includes nominal interface conformance and folds during specialization:

```flecha
structure Concrete << Bar, Baz {
};

Concrete value;

value.isof<Concrete>; // true
value.isof<Bar>;      // true
value.isof<Foo>;      // true through Bar
value.isof<Baz>;      // true

&Foo erased = value as &Foo;
erased.isof<Foo>; // true from its static interface type
erased.isof<Baz>; // false; the query does not inspect hidden Concrete
```

These are static queries. Erasing a concrete value to an interface reference
does not retain a general runtime type-query facility.

## Calls and dispatch

Calls on a concrete structure use its most-specific effective implementation.
Calls through an interface reference dispatch through that interface's table.
A default or final interface body may call virtual/default members, and those
calls continue to dispatch to the concrete implementation:

```flecha
interface Action {
    error run(&self) virtual;
    error other(&self) final;
};

error Action::other(&self) {
    return self->run();
}
```

An overriding body may call a specific inherited default with a qualified
call:

```flecha
error Derived::run(&self) override {
    return Base::run(self);
}
```

There is no automatic parent-body invocation. A new default or final replaces
the effective provider for its descendants.

## Runtime representation

Interface conformance does not change concrete value layout. A concrete value
contains no hidden interface-table pointer. A checked interface reference is
conceptually two machine words:

```text
InterfaceRef<I> {
    data:  pointer to the concrete value,
    table: pointer to Table<Concrete, I>,
}
```

The tables are immutable globals with static storage. Forming, passing,
returning, casting, or upcasting an interface reference does not allocate.

### Composed subtables

Flecha v0.4 lowers each root conformance to one composed table. Parent
interfaces are embedded as subtables, followed by slots originated locally:

```text
Table<Concrete, Interface> {
    parent subtables in declared parent order
    local slots in source declaration order
}
```

Consider:

```flecha
interface Foo {
    error foo(&self) virtual;
};

interface Bar << Foo {
    error foo(&self) virtual;
    error bar(&self) virtual;
};

interface Baz {
    error baz(&self) virtual;
};

structure Gee << Bar, Baz {
};
```

The compiler emits independent root tables conceptually shaped as:

```text
Gee-as-Bar {
    Foo subtable {
        foo -> Gee::foo
    }

    Bar local {
        bar -> Gee::bar
    }
}

Gee-as-Baz {
    Baz local {
        baz -> Gee::baz
    }
}
```

There is no required `Gee-all-interfaces` mega-table. The `Foo` view points
directly to the embedded `Foo` subtable inside `Gee-as-Bar`.

A vertical redeclaration does not append a slot. It updates the effective
declaration and selected provider for the canonical slot in its origin
subtable. `virtual`, `default`, and `final` are compile-time states and consume
no runtime table fields.

The safe-composition rules guarantee:

- every ancestor occurs exactly once under one root conformance;
- every canonical slot occurs exactly once in that root table;
- every interface-to-ancestor conversion has one path; and
- every ancestor subtable has one statically known offset.

A concrete-to-interface cast selects the relevant root table or embedded
subtable directly:

```text
g as &Bar:
    data  = address of g
    table = address of Gee-as-Bar

g as &Foo:
    data  = address of g
    table = address of Gee-as-Bar.Foo

g as &Baz:
    data  = address of g
    table = address of Gee-as-Baz
```

An interface-to-parent upcast preserves `data` and adjusts only `table` by a
compile-time-known offset:

```text
foo.data  = bar.data
foo.table = address of bar.table.Foo
```

Dispatch loads the function pointer from a statically known subtable and slot.
It performs no name, interface-ID, or concrete-type lookup.

### Linkage and generic specialization

Nested tables are ordinary immutable globals. At object-file level, references
to an embedded subtable lower to normal symbol-plus-offset relocations; the
linker does not need to understand interface semantics.

Each closed generic specialization used through an interface receives its own
root tables. Those tables and their interior references use the same linkage,
deduplication, and COMDAT policy as the specialization's other generated
artifacts.

Direct concrete calls may remain statically dispatched. Interface calls are
indirect. No garbage collector, heap box, embedded base object, runtime type
registry, or general reflection facility is introduced.

## Behavior-derived conformance

The proposed [v0.4 behavior model](behaviors-v0.4.md) may add conformance to a
decorated structure or enum and supply receiver implementations. Structure
behaviors may additionally use typed field metadata. This does not create a
second interface system.

Explicit and behavior-derived root conformances are combined before validation
and must satisfy the same antichain, unique-path, and disjoint-`MethodKey`
rules. A behavior cannot use a generated method to repair an invalid
composition edge.

A behavior may implement only methods reachable from interfaces in its
declared conformance list. Every body that implements or replaces an interface
slot uses `override`, repeats `protected` when required, and matches the full
contract. An exact handwritten implementation may replace its derived
counterpart according to the behavior conflict rules, but it does not weaken
interface composition or finality.

## Diagnostics and validation order

The compiler registers interface declarations, composition edges, concrete
types, behavior-derived edges, and method signatures before validating bodies.
It then:

1. rejects interface-composition cycles;
2. rejects duplicate direct parents and parent lists that are not antichains;
3. rejects shared ancestors and all diamond paths;
4. rejects horizontal `MethodKey` collisions before selecting bodies;
5. builds canonical vertical lineages and validates exact contracts;
6. validates the `virtual -> default -> final` state machine, including direct
   `virtual -> final` transitions;
7. matches out-of-line bodies and validates `override`, visibility, borrow,
   ownership, and effects;
8. checks every concrete type for unresolved virtual requirements and illegal
   final replacements;
9. builds composed table layouts and cast paths; and
10. type-checks interface, behavior, and concrete bodies normally.

A missing concrete implementation is reported at the relevant conformance
edge with the complete required signature and its most-specific declaring
interface. A mismatching candidate is included as related context.

Composition diagnostics should distinguish redundant parents, ancestor
diamonds, and method-slot collisions. Dedicated diagnostics also cover invalid
category regression, missing or extraneous `override`, protected-visibility
mismatch, illegal interface-value storage, invalid casts, and borrow conflicts.

## Deferred beyond v0.4

The following are deliberately outside this contract:

- generic interfaces and generic interface methods;
- owning interface values and allocation-backed existential boxes;
- runtime downcasts, sideways casts, and dynamic `isof` queries;
- interface fields, constants, lifecycle declarations, and static methods;
- owning and raw-pointer interface receivers;
- conditional conformance inferred from generic method bodies;
- a stable cross-compiler interface-table ABI;
- runtime interface reflection or a general type registry; and
- unrestricted compile-time reflection or metaprogramming.

Behavior derivation is specified separately in
[behaviors-v0.4.md](behaviors-v0.4.md). It must not weaken the interface
identity, composition, transition, visibility, borrow, dispatch, or allocation
contracts specified here.
