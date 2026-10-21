# Structures and lifecycle methods

Structures are nominal inline value types. This milestone implements fields,
privacy, out-of-line methods, construction, automatic destruction, and
whole-value moves for noncopyable movable structures. It does not add
inheritance or `unsafe structure`; explicit raw heap allocation is documented
separately.

## Declaration and access

```flecha
structure Box {
    i32 x;
    private {
        i32 y;
        bool ready;
    }
};

Box Box::init() override {
    return build Box {
        self.x = 2;
        self.y = 40;
        self.ready = true;
    };
}

private i32 Box::hidden(&const self) { return self->y; }
i32 Box::value(&const self) { return self->x + self->hidden(); }
void Box::set(&self, i32 value) { self->y = value; }
```

The declaration ends with `;`. Fields and methods are public by default.
`private T field;` and a `private { ... }` field group are equivalent. A private
member is accessible only inside methods belonging to its structure, including
access through another instance. A build block does not grant private access
by itself. Fields and methods cannot share names; `init`, `zero`, and `tear` are
reserved lifecycle names. Types may refer to later structure declarations.

Structure values use `.`, while references and pointers use `->`, for both
fields and methods:

```flecha
Box box = Box::init();
box.set(40);
{
    ref := &box;
    ref->set(41);
}
unsafe {
    *Box ptr = &raw box;
    ptr->set(40);
}
```

The first receiver parameter can be `&self`, `&const self`, `*self`, or
`*const self`. Its target is the method's structure. Explicitly writing
`&Box self` is equivalent. A method without a receiver is static and uses
`Box::method()`. There is no method overloading in this milestone.

Reference receivers participate in checked borrowing. The implicit receiver
borrow lasts throughout argument evaluation and the call. Field borrows
conservatively borrow the whole containing structure; disjoint field splitting
is not yet supported. A read-only receiver cannot call a mutable method or
write fields. A raw pointer receiver requires unsafe permission. Unsafe methods
obey the ordinary lexical unsafe-call rules. Checked references do not implicitly
convert to raw pointer receivers.

## Initialization

Every structure has a generated `Box::init()` equivalent to:

```flecha
Box Box::init() { return build Box {}; }
```

Users replace it with an override that returns the structure and has no `self`
receiver. Overrides may take arguments:

```flecha
Box Box::init(i32 x, i32 y) override {
    return build Box { self.x = x; self.y = y; };
}
// Within a function:
Box box = Box::init(2, 40);
```

There is one init per structure; this replaces the generated zero-argument
constructor rather than adding an overload. Ordinary argument type, borrowing,
privacy, and unsafe checks apply. `Box::init()` is an error if the override
requires arguments. Nested value fields need explicit declaration defaults in
that case, for example `Box box = Box::init(2, 40);`. The same applies to arrays
and tuples containing Box. Raw pointer fields need no constructor arguments;
they default to null. Reference fields remain unsupported pending lifetime
contracts. Additional named static factories may also accept parameters.

## Default construction and `zero`

`none` constructs the expected structure default in declarations,
assignments, arguments, returns, and ordinary `==`/`!=` comparisons. A plain
structure receives a generated `zero()` when every field can be defaulted:
numbers become zero, booleans become false, characters become `\0`, raw
pointers become null, fixed arrays default every element, and nested plain
structures recursively default their fields. This is semantic initialization,
not a bytewise memory clear.

A structure with a custom `init` or `tear` must declare its default explicitly:

```flecha
structure Position {
    i32 line;
    i32 column;
};

Position Position::zero() override {
    return build Position {
        self.line = 1;
        self.column = 1;
    };
}
```

`zero` is static, takes no parameters or `self`, returns its owning structure,
and is always safe. It cannot allocate, free, enter an unsafe block, perform
ownership operations, cause output or another external side effect, or hide
one of those operations behind a helper call or an implicit cleanup. Recursive
default construction, including indirect cycles, is rejected.
`Position::zero()` may also be called explicitly. Exporting a structure exposes
its default construction, while the override may initialize private fields.

Custom defaults and plain defaults containing them are nontrivial. The compiler
emits one warning per such structure, for example:

```text
warning: structure 'Position' is not trivially zeroed; default construction uses 'Position::zero()'
```

Structure equality is field-by-field and requires every field to be comparable.
Each operand is evaluated once, and equality neither copies nor consumes an
existing structure. A contextual `none` temporary follows normal destruction.
Strict comparison keeps `none` distinct: a structure is never `=== none`, and
that comparison does not invoke `zero`.

`build Box { ... }` initializes fields in declaration order,
then executes its block with a value named `self`, so field access there uses
`self.field`. It does not call Box's own init again.

A field may have `= expression` as its declaration default. Defaults are
reevaluated for each construction. Otherwise scalars and errors receive their
zero/no-error value, pointers receive null, and owning strings receive an empty
value. Nested structures call their own init; arrays and tuples initialize each
contained element. Unsafe default construction propagates to a generated init.
Field default expressions are checked in global safe context; use a suitable
factory for more complex construction.

All fields are initialized before the build body. Const fields must therefore
use declaration defaults if zero is unsuitable, and cannot be assigned inside
the body. Reading and updating mutable fields is allowed. The unfinished whole
`self` cannot be copied, passed to another function, or receive method calls.
Borrows of construction fields must stay inside the build block.
Build bodies allow local declarations, conditionals and local loops. `return`,
`jumpto`, positions, and exits targeting an outer loop are rejected inside a
build; validate fallible preconditions before constructing. This milestone does
not implement partial construction or unwinding an unfinished value.

Structure variables require an initializer. Inline recursive layouts are
rejected; recursive raw pointer
fields such as `*Node next` are allowed. Fields support scalars, errors, raw
pointers, owning strings, structures, fixed arrays of supported array elements,
tuples of supported fields, owning slices (`own []T`), and borrowed slices
(`[]T` or `[]const T`). Owning slices are destroyed automatically with the
containing structure. A structure containing a mutable slice is noncopyable;
one containing only shared `[]const T` views may be copied.

A borrowed-slice field is tied to the same source tracked by the ordinary
borrow-result contract. Constructors and factories returning such a structure
must declare `<- parameter>`, and the compiler rejects local escapes or access
that conflicts with a live mutable field borrow:

```flecha
structure Writer {
    []u8 destination;
    usize position;
};

Writer Writer::init([]u8 destination) <- destination override {
    return build Writer {
        self.destination = destination;
        self.position = 0;
    };
}
```

Reference and `stringv` fields remain unsupported.

## Destruction, copying, and stable storage

Every structure has an implicit empty tear hook. Users replace it with:

```flecha
void Box::tear(&self) override {
    // Optional cleanup before the fields are destroyed.
}
```

`tear` is compiler-only: calling it explicitly is forbidden, including from
other methods of the same type. It must return void and take exactly one mutable
reference receiver. Private tear overrides remain callable by compiler cleanup.

At scope exit the compiler runs deferred calls in reverse registration order,
then destroys live owned slots in reverse registration/declaration order. For
one structure it invokes the custom tear, then destroys fields in reverse
declaration order. Arrays and tuples destroy elements in reverse order. Raw
pointer and reference targets are not destroyed. Cleanup runs on fallthrough,
return, break, proceed and outward jumps; runtime termination does not unwind.
Live flags prevent cleanup of slots skipped by a branch or jump and prevent
repeated cleanup after a previous exit. Reexecuting a declaration destroys its
previous live value before constructing its replacement.

Structures with only generated tear hooks and copyable fields may be copied,
assigned, returned, destructured, and passed by value. Owning string fields are
deep-copied, including within arrays and tuples. Deferred function, callable,
and member calls capture these values independently at registration and release
them after the call. A checked method receiver remains borrowed through scope
exit.

A custom tear makes its structure noncopyable, transitively through fields,
arrays, and tuples. If the tear is safe, the complete value is nevertheless
movable. Initialization, assignment, by-value arguments, returns, discarded
expressions, match arms, complete tuple destructuring, and whole fixed-array
transfers implicitly move such values. The source becomes unavailable and is
not torn down; assigning a fresh value restores it. Replacing a destination
evaluates the new value before destroying the old one, and direct self-move is
rejected. Fresh build, factory, and match results construct directly when
possible rather than being copied through an extra owner.

Moves transfer cleanup responsibility exactly once. They are destructive
accesses for borrow checking, so a value cannot move while a reference, slice,
returned loan, or deferred borrow still protects it. Moving an individual
field or indexed element out of an aggregate is not supported; move the whole
named value or use a type-specific replacement API.

An **unsafe tear additionally makes the type immovable**. Creating a value of
that type requires unsafe permission, including through an otherwise safe
factory. Fresh build and factory results construct directly into their final
storage, including nested fields, arrays, and tuple returns. The compiler does
not relocate them between construction and destruction. This authorizes the
later automatic tear; cleanup still occurs when the owning lexical scope ends,
which need not be the unsafe block where the value was constructed.

## Diagnostics and native representation

- `FLECHA-S0043`: invalid structure, receiver, layout, or construction.
- `FLECHA-S0044`: private member access.
- `FLECHA-S0045`: invalid lifecycle signature/override or direct tear call.
- `FLECHA-S0046`: prohibited copy, partial move, immovable transfer, or
  self-move.
- `FLECHA-S0059`: use of a value that was moved, or may have moved on another
  control-flow path.

Existing unsafe, const, borrow, and definite-assignment diagnostics continue to
apply. Native representation is a named non-packed LLVM structure with fields
in declaration order. Results containing structures use caller-provided output
storage; this compiler-private convention is described in
[runtime_abi.md](runtime_abi.md). It is not a portable C aggregate ABI.

Run `examples/structures.fl` for a complete example (exit status 42).
`NativeSemanticTests.Structures` checks methods, privacy-related valid paths,
address stability, nested destruction order, all scope exits, owning-field
copies, whole-value moves, defaults, and deferred captures/results. Parser and
semantic tests cover rejected forms and diagnostics. The sanitizer target
includes the native fixture.
