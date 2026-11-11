# Interfaces for Flecha 0.4

Status: proposed language design; not implemented

Interfaces define nominal callable contracts without adding storage,
implementation inheritance, or hidden allocation. Structures and enums
conform to one or more interfaces with `<<`, and interfaces compose other
interfaces with the same syntax. Composition forms a directed acyclic graph: a
declaration may have several parents, and one ancestor may be reachable through
several paths.

## Declaration model

An interface contains function signatures only. It cannot contain fields,
constants, storage, or lifecycle state.

```flecha
export interface Service {
    error start(&self) virtual;
    error status(&const self) default;
    error stop(&self) final;

    protected error validate(&const self) default;
};
```

Every interface method declares exactly one method category:

- `virtual` has no interface body and remains a requirement;
- `default` has an interface body and may be overridden; and
- `final` has an interface body and may not be overridden.

An interface signature without one of these categories is invalid. A
`virtual` method must not have a body. Every `default` and `final` method must
have exactly one out-of-line body.

Interface declarations do not contain inline bodies. Default and final bodies
follow the existing out-of-line Flecha method form:

```flecha
error Service::status(&const self) {
    return none;
}

error Service::stop(&self) {
    return none;
}

error Service::validate(&const self) {
    return none;
}
```

This proposal specifies receiver methods. Static functions do not participate
in interface conformance: an interface reference has no receiver-based way to
select one, while type-constrained static dispatch and factories would require
a special `Self` type. Type-level construction instead belongs in an ordinary
generic function constrained by the receiver interface. Interface fields,
constants, `init`, `zero`, and `tear` are also outside the v0.4 contract.

## Visibility

Interface methods are public unless declared `protected`. A protected method is
accessible from:

- method bodies belonging to the declaring interface;
- method bodies belonging to descendant interfaces; and
- methods belonging to structures or enums that conform to the interface,
  directly or transitively.

Unrelated callers cannot name a protected interface method. An override
inherits the interface method's visibility, so omitting `protected` on an
out-of-line override does not accidentally make it public. A public inherited
method cannot be narrowed to protected.

`private` interface methods are not part of this proposal. Flecha's existing
concrete-type private rule remains unchanged: a private member belongs to one
exact owning type. The interface use case that permits conforming types to call
a hidden method is spelled `protected` instead.

## Structure conformance

A structure declares nominal conformance after its name:

```flecha
export structure Worker << Service {
    i32 identifier;
};
```

`<<` in a declaration header is the conformance operator. It remains the
shift-left operator in expressions. Conformance is not implementation
inheritance: it adds no base subobject, fields, constructor or destructor
ordering, lifecycle hooks, or hidden pointer to `Worker`.

A structure may conform to several interfaces:

```flecha
structure Foo << IBar, IGee {
};
```

`<<` may appear only once in one declaration header. Its right-hand side is a
comma-separated list of that declaration's direct interfaces. Chaining the
operator in one header is invalid:

```flecha
structure Gee << Bar << Foo {
}; // Error: chained conformance is ambiguous.
```

That spelling could mean either that `Gee` conforms independently to `Bar` and
`Foo`, or that declaring `Gee` retroactively makes `Bar` conform to `Foo`.
Flecha permits neither interpretation. Use commas for sibling interfaces and
declare every transitive edge on its actual owner:

```flecha
interface Bar << Foo {
};

structure Gee << Bar {
};
```

Here `Gee` conforms transitively to `Foo`, while the nominal `Bar << Foo` edge
remains stable regardless of where `Gee` is declared or used.

The compiler collects all methods before validating conformance, so required
overrides may appear after the structure declaration:

```flecha
error Worker::start(&self) override {
    return none;
}
```

An override must match the inherited method's complete signature, including
its name, receiver form and constness, parameter and result types, generic
parameters, ownership contracts, borrow-result contracts, safety, and other
call effects. A differently shaped method is an overload and does not satisfy
the interface requirement.

## Enum conformance

Enums are receiver-bearing concrete value types and may conform to the same
interfaces. When a backing type is present, the conformance list follows it:

```flecha
export enum State: u8 << Displayable {
    Off,
    On,
}

error State::display(&const self, &Writer writer) override {
    // Ordinary enum method body.
    return none;
}
```

Without an explicit backing type, `<<` follows the enum name. An enum header
uses one `<<` followed by its comma-separated direct interfaces; chained
conformance is rejected exactly as for structures.

Enum conformance changes neither the backing type nor its scalar runtime
representation. Enum receiver implementations follow the ordinary enum method
rules. A behavior may derive the same conformance edge for a decorated enum;
behaviors receive no enum-variant or discriminant reflection.

## Interface composition

Interfaces compose other interfaces with the same operator:

```flecha
interface Foo {
    error foo(&self) virtual;
};

interface Bar {
    error bar(&self) default;
};

interface Gee << Foo, Bar {
    error gee(&self) final;
};

error Bar::bar(&self) {
    return none;
}

error Gee::gee(&self) {
    return none;
}
```

Composition is transitive. A concrete type conforming to `Gee` also conforms
to `Foo` and `Bar`. Composition cycles are invalid, and parent order never
chooses an implementation.

The same canonical declaration inherited through more than one path is
deduplicated:

```flecha
interface Root {
    error close(&self) final;
};

interface Left << Root {
};

interface Right << Root {
};

interface Combined << Left, Right {
}; // Root::close is inherited once.

error Root::close(&self) {
    return none;
}
```

This is a directed acyclic graph rather than a strict tree. There is no diamond
of stored base objects because interfaces contain no storage.

## Effective method state

For one exact method signature, the most-specific interface declaration
determines the state seen by descendants and concrete types:

| Effective state | Body | Concrete type obligation |
| --- | --- | --- |
| `virtual` | forbidden | must override |
| `default` | required | may override |
| `final` | required | must not override |

If a child interface does not mention an inherited method, it inherits the
parent's effective state. Repeating the method as `virtual` explicitly keeps
or restores an unresolved requirement:

```flecha
interface Foo {
    error run(&self) virtual;
};

interface Gee << Foo {
    error run(&self) virtual;
};

structure Concrete << Gee {
};

error Concrete::run(&self) override {
    return none;
}
```

A child may also re-abstract an inherited default:

```flecha
interface Foo {
    error run(&self) default;
};

error Foo::run(&self) {
    return none;
}

interface Gee << Foo {
    error run(&self) virtual;
};
```

Every concrete type conforming to `Gee` must now override `run`. The older
default remains available only through an explicit qualified call such as
`Foo::run(self)`.

Declaring the child method as `default` supplies an overridable implementation:

```flecha
interface Gee << Foo {
    error run(&self) default;
};

error Gee::run(&self) override {
    return none;
}

structure Concrete << Gee {
}; // Complete: Concrete may, but need not, override run.
```

Declaring it as `final` supplies the last implementation in the chain:

```flecha
interface Gee << Foo {
    error run(&self) final;
};

error Gee::run(&self) override {
    return none;
}

structure Concrete << Gee {
}; // Complete: Concrete must not override run.
```

An exact `Concrete::run` definition is an error in the final case whether or
not it is marked `override`. A differently shaped overload remains permitted.

## Placement of `override`

`override` belongs to an out-of-line implementation, not to an interface
signature declaration. The declaration records whether the resulting method
is virtual, default, or final; the implementation records whether its body
replaces an inherited slot.

```flecha
interface Local {
    error inspect(&self) default;
};

error Local::inspect(&self) {
    return none;
}
```

The new method above must not use `override`. When a child supplies a body for
an inherited virtual or default method, `override` is mandatory:

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

Replacing an inherited final method is always invalid.

## Multiple-parent conflicts

Exact virtual requirements inherited from several parents merge into one
requirement. One concrete override satisfies every merged slot.

One inherited default or final implementation may satisfy matching virtual
requirements from other parents. Two unrelated provided bodies for the same
exact signature are ambiguous; parent order does not select one. A composing
interface must resolve two defaults with a more-specific declaration and body:

```flecha
interface Foo {
    error run(&self) default;
};

interface Bar {
    error run(&self) default;
};

interface Gee << Foo, Bar {
    error run(&self) default;
};

error Gee::run(&self) override {
    error err = Foo::run(self);
    if err != none {
        return err;
    }

    return Bar::run(self);
}
```

The one `Gee::run` body resolves both inherited slots. A composing interface
may instead redeclare the method as virtual and leave the requirement for its
concrete structures.

Two unrelated final bodies with the same exact signature are incompatible.
Likewise, a final body and a different provided body cannot be resolved by a
descendant because the final body cannot be replaced. The same canonical final
declaration arriving through multiple graph paths is not a conflict.

## Interfaces are not value types

An interface has no value representation and cannot be instantiated, copied,
moved, built, or stored by value, including as a structure field. The initial
runtime-facing form is a checked interface reference:

```flecha
&Interface
&const Interface
```

A checked reference to a conforming concrete value converts implicitly to the
corresponding interface reference:

```flecha
error operate(&Service service) {
    return service->start();
}

Worker worker = Worker::init();
error err = operate(&worker);
```

Taking the address remains explicit. `operate(worker)` is invalid. Mutable and
const conversions follow the ordinary checked-reference rules: a const
concrete value cannot produce a mutable interface reference.

The interface loan retains the original concrete borrow origin. It cannot
outlive the concrete value, and a mutable interface reference excludes the
same conflicting accesses, moves, replacement, and destruction as an ordinary
mutable reference. Returned interface references use the normal borrow-source
contract.

## Reference conversions

The following conversions are implicit and allocation-free:

- `&Concrete` to `&Interface` when `Concrete` conforms to `Interface`;
- `&Concrete` to any transitively composed parent interface;
- `&ChildInterface` to `&ParentInterface`; and
- the corresponding mutable-to-const forms.

Downcasts and sideways casts are not implicit:

```flecha
Concrete concrete = Concrete::init();

&Gee gee = &concrete;
&Foo foo = gee; // Valid parent-interface conversion.

&Gee again = foo; // Invalid downcast.
&Bar bar = foo;   // Invalid sideways conversion.
```

An exact concrete-reference overload ranks ahead of an interface conversion.
If a concrete type conforms to two unrelated interface parameter types, a call
that could convert to either is ambiguous and requires an explicitly typed
interface reference at the call site.

## Static conformance queries

`isof` includes nominal interface conformance and folds during specialization:

```flecha
structure Foo << IBar, IGee {
};

Foo value = Foo::init();

value.isof<Foo>;  // true
value.isof<IBar>; // true
value.isof<IGee>; // true

&IBar erased = &value;
erased.isof<IBar>; // true from its static interface type
erased.isof<IGee>; // false; the query does not inspect hidden Foo
```

Interface conformance may also constrain a generic parameter:

```flecha
error operate<T: IBar>(&T value) {
    return value->bar();
}
```

These are static queries. Erasing `&Foo` to `&IBar` does not retain a general
runtime type-query facility. An `&IBar` therefore does not dynamically report
that its hidden concrete object also conforms to `IGee`, and it cannot convert
sideways to `&IGee`. Runtime type metadata and interface downcasting are not
part of this proposal.

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

An override may call a specific inherited default directly with a qualified
call:

```flecha
error Concrete::run(&self) override {
    return Foo::run(self);
}
```

## Runtime representation and allocation

Interface conformance does not change a structure's layout or an enum's scalar
representation. A concrete value contains no hidden interface-table pointer.
The compiler emits static dispatch tables for the concrete/interface pairs that
the program uses.

A checked interface reference is conceptually two machine words:

```text
{
    data:  pointer to the concrete value,
    table: pointer to the concrete/interface dispatch table,
}
```

The table has static storage. Forming, passing, returning, or converting an
interface reference does not allocate. Direct concrete calls may be statically
dispatched; calls through interface references are indirect. Parent-interface
upcasts select the corresponding static parent table without inspecting the
concrete type dynamically.

No garbage collector, heap box, embedded base object, runtime type registry, or
general reflection facility is introduced by this model.

## Behavior-derived conformance

The proposed [v0.4 behavior model](behaviors-v0.4.md) may add conformance to a
decorated structure or enum and supply receiver implementations. Structure
behaviors may additionally use typed field metadata. This does not create a
second interface system. Derived edges and bodies pass through the same
composition, exact-signature, `override`, visibility, finality, borrow,
dispatch, and allocation rules described here.

A behavior may implement only methods reachable from interfaces in its
declared conformance list. It cannot invent unrelated target methods. An exact
handwritten implementation wins over its derived counterpart and is validated
as an ordinary override.

## Diagnostics and validation order

The compiler registers interface declarations, composition edges, concrete
type declarations, and method signatures before validating bodies and
conformance. It then:

1. rejects interface-composition cycles and visibility leaks;
2. deduplicates canonical declarations inherited through multiple paths;
3. computes the most-specific effective state for every exact signature;
4. reports unresolved or conflicting parent implementations;
5. matches out-of-line bodies and validates `override` placement;
6. checks every concrete type for remaining virtual requirements; and
7. type-checks interface and concrete bodies normally.

A missing concrete implementation is reported at the relevant `<< Interface`
conformance edge with the complete required signature. A mismatching candidate
is included as related context. Final-method replacement, ambiguous defaults,
invalid visibility narrowing, and illegal interface-value storage receive
dedicated diagnostics.

## Deferred questions

The following are deliberately outside this initial proposal:

- owning interface values and allocation-backed existential boxes;
- runtime downcasts, sideways casts, and dynamic `isof` queries;
- interface fields, constants, and lifecycle declarations;
- static interface requirements;
- a stable cross-compiler interface-table ABI;
- unrestricted compile-time reflection or metaprogramming.

Behavior derivation is specified separately in
[behaviors-v0.4.md](behaviors-v0.4.md). It must not weaken the interface
signature, override, visibility, borrow, dispatch, or allocation contracts
specified here.
