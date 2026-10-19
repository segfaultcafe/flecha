# Raw-pointer ownership contracts

`own` and `lend` describe cleanup responsibility at function boundaries and in
structure fields. They do not change the raw-pointer representation or ABI.
`const` still controls mutation. Plain raw-pointer types remain ownership-neutral.
References use their existing checked-access rules and never take either keyword:
`own &T` and `lend &T` are invalid.

```flecha
unsafe void release(own *i32 pointer) { free(pointer); }
unsafe i32 read(lend *const i32 pointer) {
    if pointer == null { return 0; }
    return *pointer;
}
unsafe (own *i32, error) create() { return new<i32>(42); }
unsafe own *i32 forward(own *i32 pointer) { return pointer; }
```

## Parameters and returns

A `lend` parameter permits access during the call. It may be forwarded to another
lender, but cannot be freed, consumed, returned, or retained in escaping storage.
Local aliases retain the same restrictions. Lending a raw pointer does not create
a checked reference or impose reference-style exclusivity.

An `own` parameter transfers responsibility unconditionally at the call boundary.
Every normal callee exit must free the allocation, forward it to another consumer,
store it in an owning field, or return it through an owning result. An error does
not restore the caller's ownership: APIs that give it back must return an owning
result explicitly. This also applies to recursive consuming calls.

Transfer makes existing tracked aliases unavailable for access or cleanup. It
does not null their runtime storage. Passing the same allocation simultaneously
as an owner and another argument is rejected. A pending deferred use or cleanup
prevents conflicting transfer. Deferred calls retain the existing capture and
execution timing: `defer release(pointer)` schedules consumption for scope exit.

An owning return transfers cleanup responsibility to its caller. Ignoring a live
owning result is an error. Tuple results can annotate individual pointer elements,
including nested tuple results. Owning or lending tuple parameters, nested
pointer contracts, local contract declarations, and lending returns are not
supported. In an annotated function, returning tracked ownership through an
unannotated pointer result is rejected.

Null carries no cleanup obligation. Passing or returning null as an owner is valid;
ownership does not guarantee non-nullness. Visible factory return paths preserve
allocation-error/null relationships, including `new<T>` failure paths. An arbitrary
error result does not imply that an accompanying owning pointer is null.

## Anonymous-function captures

Raw allocations use explicit capture contracts, parallel to parameter
contracts but tied to the closure environment:

```flecha
unsafe func Reader() => (i32);

unsafe i32 use() {
    borrowed, borrowError := new<i32>(20);
    if borrowError != none { return 1; }
    i32 result = 0;
    {
        Reader reader = unsafe [lend borrowed]() => { return *borrowed; };
        result = reader();
    }
    free(borrowed);

    retained, retainError := new<i32>(22);
    if retainError != none { return 2; }
    Reader owner = unsafe [own retained]() => { return *retained; };
    return result + owner();
}
```

`lend` requires an `unsafe` anonymous function and a raw pointer or
runtime-sized buffer. Raw-resource `own` has the same safety requirement. A
tracked allocation cannot use `copy`, because copying the address does not say
which value owns cleanup. Captured bindings are read-only; mutation through a
mutable pointer remains possible.

`lend name` copies the raw handle without transferring its cleanup obligation.
The closure keeps that allocation lent until the callable leaves its creation
scope, so the owner cannot be freed or passed to an `own` consumer in the
meantime. A lending closure cannot be returned or stored into a longer-lived
binding or container.

`own name` consumes the source owner's cleanup obligation when the closure is
created. Moving the callable moves that obligation. Its destroy thunk releases
the captured allocation in reverse capture order, then frees the environment.
Null remains valid and needs no cleanup. The callable may be invoked repeatedly,
so its body cannot free the capture or pass it to an `own` parameter; otherwise
the next invocation could use released storage. Such consuming bodies require a
future single-use callable design.

The same `own name` syntax moves noncopyable safe values into an environment:

```flecha
func Mapper(i32) => (i32);

Mapper decorate(Mapper inner) <- inner {
    return [own inner](value) => { return inner(value) + 1; };
}
```

Callable values, mutable checked handles, and movable aggregates such as tuples,
fixed arrays, structures, and safe containers are accepted. Copyable values use
`copy` instead. A structure with unsafe teardown is immovable and rejected. Safe
owning captures do not require an unsafe literal, even when an aggregate hides
raw storage behind safe teardown. The move invalidates the source binding,
retains any checked-reference origins, and recursively destroys the value in
reverse capture order. A reusable closure may call or inspect this state but may
not move it out or pass it to a consuming parameter.

## Owning fields and tear

```flecha
structure Box { own *i32 value; };
unsafe Box Box::init(own *i32 value) override {
    return build Box { self.value = value; };
}
void Box::tear(&self) override { unsafe { free(self->value); } }
```

Owning fields require an explicit `tear`. The compiler independently checks that
its normal exits discharge every live owning field. `tear` explicitly frees or
transfers the stored allocation; destroying the raw-pointer field performs no
additional free. Null fields need no cleanup.

Existing lifecycle rules remain: `tear` is compiler-invoked and cannot be called
explicitly. A custom safe tear makes its structure noncopyable but movable as a
complete value; moving transfers the field cleanup obligation and invalidates
the source until reassignment. An unsafe tear makes the structure immovable.
Transferring a pointer to an immovable structure does not move the structure.
Fresh construction still goes directly into its final storage. Moving an owning
field out by ordinary member access remains unsupported; use `take` where its
explicit field-replacement contract applies.

The initial field implementation supports initialization in `build` and access
through the structure's `self` methods. Omitted owning fields start null, as other
raw-pointer fields do. Methods other than `tear` ordinarily receive lent access to their
owning fields. The explicit unsafe `take(self->field)` operation now transfers
ownership out and sets the field to null, allowing a replacement to be assigned.
Taking an address of an owning field, replacing a live field, declaration
initializers, and direct access through other receivers are rejected. Extraction
invalidates earlier aliases and requires the extracted owner to be discharged. See [dynamic arrays](dynamic_arrays.md) for replacement rules.

## Checking boundaries

Annotated functions are verified independently, even when unused. Calls use their
contracts, so annotated recursion does not require recursive body interpretation.
Named callable signatures carry the same `own` and `lend` parameter contracts
through indirect calls. An owning argument is transferred at the indirect call
boundary, and an owning result creates a new caller cleanup obligation.
Known-null calls are checked against the body as well. Unannotated helpers retain
the existing bounded visible-body analysis. Tracked raw pointers passed to an
annotated function need explicit parameter contracts; indirect mutation through
an address of a tracked pointer slot is not yet summarized.

The existing allocation-analysis path budgets remain, with an additional limit of
128 nullable call configurations per compilation unit. Repeating an owning return
site with its previous result still live is rejected. Unsupported ownership effects
fail before object emission rather than silently losing tracking.

This does not introduce external-function support, safe heap references, partial
field/index moves, or complete memory-safety verification. Whole noncopyable
aggregate moves are implemented, but unverified external raw-pointer validity,
aliasing, nullability, and stack lifetimes remain unsafe obligations.
Runtime-sized zeroed allocation and the standard-library dynamic arrays are
implemented; see [dynamic arrays](dynamic_arrays.md). Partially initialized
storage and move-only dynamic-array element relocation remain future work.
