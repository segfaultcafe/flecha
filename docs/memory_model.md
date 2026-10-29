# Memory model and remaining proposals

Status: implemented behavior is identified below; broader ownership sections
remain design proposals.

The agreed `own` / `lend` contracts are now implemented as described in
[ownership contracts](ownership_contracts.md). References never take either
keyword. Whole-value move-by-default semantics are implemented for
noncopyable aggregates with safe teardown. Existing standalone string copies
remain unchanged, and aggregates with an unsafe tear remain immovable.

The implemented model includes explicit raw allocation/free in unsafe code,
`defer` for scheduled cleanup, and mandatory diagnostics for tracked resource
errors. Unsafe code retains caller obligations; this is not a guarantee that
arbitrary raw-pointer programs are memory-safe. Mutable and shared references
use `&T` and `&const T`, with `&value` creation and `*reference` access. The
preferred fixed-binding spellings are `constptr T` for `const *T` and
`constref T` for `const &T`. Existing string copy semantics remain unchanged.

Pointers, fixed arrays, checked references and slices, borrowed-result
contracts, raw allocation, ownership contracts, `own []T` safe heap owners,
aggregate moves, and deferred direct calls are implemented. Their focused
feature guides are normative for the current compiler. Custom allocators,
partial moves, and moving elements within dynamic containers remain proposals. Current
behavior is recorded in [definite assignment](definite_assignment.md),
[strings](string.md), [const declarations and types](const_types.md),
[checked references](checked_references.md), [checked slices](checked_slices.md),
and [raw allocation](raw_allocation.md).

## 1. Storage, ownership, and access are separate

A value's storage location does not determine whether it owns a resource.
An owning value can live on the stack while owning a heap allocation. A raw
pointer can point at stack, heap, static, or external storage without owning
any of it.

| Category | Responsibility | Assignment and passing |
| --- | --- | --- |
| Scalars | No resource cleanup | Copy the value |
| Raw pointer `*T` / `*const T` | No ownership or lifetime guarantee | Copy the address |
| Shared reference `&const T`, shared slice `[]const T`, or `stringv` | Borrow existing storage | Copy the view, preserving its lifetime restrictions |
| Mutable reference `&T` or mutable slice `[]T` | Borrow storage for exclusive access | Transfer the borrow; do not duplicate exclusive access |
| Standalone owning string | Release its allocation exactly once | Copy with independent storage; existing string semantics are unchanged |
| Owning slice `own []T` | Release its runtime-sized buffer automatically | Move the owner; borrow it with slicing |
| Noncopyable aggregate with safe tear | Run typed teardown exactly once | Move the complete value and transfer cleanup responsibility |
| Aggregate with unsafe tear | Run address-sensitive teardown exactly once | Immovable after construction; borrow it instead |
| Fixed-size array | Store elements inline; own their element values | Copy if every element is implicitly copyable; otherwise transfer the array |

An ordinary `[]T` slice is only a borrow; `own []T` is its explicit owning
counterpart. Neither form is inherently growable. A raw pointer is never
implicitly freed. Shared ownership, reference counting, and garbage collection
are not part of the initial model.

## 2. Initialization and validity

A declaration may omit its initializer only when its type is trivially
zeroable. Omission requests the same observable zero value as `= none`, so
these are equivalent:

```flecha
i32 count;          // initialized to zero
i32 zero = none;    // initialized to zero
```

Types whose default construction performs work must spell `= none`
explicitly. This keeps nontrivial initialization visible at the declaration.

For raw pointer variables, the trivial zero is null. It is a valid pointer
value, but it does not designate an accessible object. Initializing a pointer
says nothing about the initialization of its pointee.

Taking a raw address requires a live variable. Explicit uninitialized buffers,
out-parameters, and tracked indirect initialization are later features.

Initially, arrays must be initialized as whole values before producing a safe
slice. Partial element initialization and partial moves of aggregates are
excluded until the compiler can track them and clean them up correctly.

## 3. Ownership and transfer — implemented whole-value moves

Noncopyable structures with safe teardown move by default as complete values.
Tuples and fixed arrays containing such structures inherit the same behavior:

- Initialization and assignment from a movable variable transfer its resource.
- Passing a movable value transfers it to the callee; returning one transfers
  it to the caller. Borrowed parameters provide non-consuming access.
- Complete tuple destructuring, match bindings, discards, and whole fixed-array
  transfers also consume a movable value. Ignored or discarded owning parts are
  still destroyed exactly once.
- The source becomes unavailable until assigned a new value. Reads, borrows,
  and a second move from a moved or possibly moved source are errors.
- Replacing an owner evaluates the replacement before releasing the old
  resource. Direct self-move assignment is rejected.
- A move is a destructive access and is rejected while a conflicting borrow,
  deferred use, or owner-backed returned view is live.
- Copyable values retain copy semantics. Standalone `string` initialization,
  assignment, and by-value argument passing continue to make independent copies.

For example:

```flecha
structure Box {
    own *i32 data;
}

void Box::tear(&self) override {
    unsafe { free(self->data); }
}

first := Box::init(...);
second := first;        // moves the complete Box
// consume(first);      // error: first was moved
first = Box::init(...); // reinitialization restores first
```

Arbitrary field and indexed-element moves are not implemented. An unsafe
teardown marks its structure, and aggregates containing it, as immovable; such
values must be constructed directly in their destination. These restrictions
avoid creating aggregate states whose cleanup cannot be proven complete.

## 4. Deterministic destruction

Every live owned resource has exactly one cleanup responsibility. Leaving its
scope releases it on fallthrough, `return`, `break`, `proceed`, and `jumpto`.
Moving a resource transfers that responsibility and prevents cleanup of the
moved-from source.

Nested scopes clean up from innermost to outermost. Within a scope, named
owners use reverse declaration order, including after backward jumps; cleanup
must consider every slot that could be live, not only earlier declarations.
Compiler temporaries also require cleanup on every applicable exit.

A join may have a resource live on only some predecessors. Definite
initialization alone is insufficient for destruction: the implementation must
also track whether each owning slot actually contains a resource at runtime
when necessary. Null storage suffices for the existing string representation;
other owning types may need explicit live flags.

Destruction cannot fail or transfer control. Immediate process termination
from a runtime safety failure need not unwind scopes. Structure teardown and
whole-value transfer are implemented as described in
[structures.md](structures.md). Partial construction and partial aggregate
moves remain future work.

## 5. Borrowed access — implemented conservative policy

Safe borrowed access must not outlive its storage or observe invalidation. The
checker uses conservative control-flow-aware lifetimes, tracks last possible
uses, and rejects cases it cannot prove safe.

- Multiple shared read-only borrows may coexist.
- A mutable borrow provides exclusive access to its borrowed region. Other
  overlapping reads, writes, or borrows are prohibited while it is active.
- While shared borrows are active, the borrowed region cannot be mutated.
- A borrowed owner cannot be moved, replaced, destroyed, or reallocated while
  that action would invalidate an active borrow.
- Reborrowing through a mutable view suspends conflicting access through the
  parent view until the child borrow ends.
- Initial checking treats the entire owner as one region. Proving disjoint
  array ranges is deferred, so some safe programs will be rejected.

`const` restricts access through a pointer or view; it does not make arbitrary
raw aliases globally immutable. Safe code must nevertheless respect the
borrow restrictions above. Raw-pointer use inside `unsafe` must uphold the
contracts of safe views that remain active.

Borrow origins are sets of possible owners. At control-flow joins, possible
origins are combined by union, while definite initialization uses intersection.
An operation must be valid for every possible origin. Loops and jumps require
a fixed point for this state as well as initialization state.

A view into a local owner or stack array cannot escape the storage lifetime.
A reference- or slice-returning function declares its single originating
parameter or receiver with the postfix `<- origin` contract. The compiler
verifies every return against that origin, rejects locals, temporaries, and
ambiguous origins, and propagates the returned loan into the caller's borrow
state. Static literal views may also escape. See
[reference and slice returns](ref_and_slice_returns.md) for the complete
contract and [strings](string.md) for borrowed string returns.

Uninitialized `stringv` declarations remain rejected. Mutable slices are
implemented with the access, lifetime, reborrow, and whole-owner region rules
described in [checked slices](checked_slices.md).

### 5.1 References for safe code

Use the agreed `&T` spelling for a mutable reference, create one with `&value`,
and access its referent with `*reference`. Use `&const T` for a shared
read-only reference. Mutable borrowing provides exclusive access under the
implemented lifetime checks. References are borrowed handles, not owners.
Reference creation does not allocate or copy the referent.

```flecha
u8 val = 3;
&u8 ref = &val;

*ref = *ref * 2; // modifies the original val, which now contains 6
```

The unary `*` accesses the referent; the binary `*` performs multiplication.
The assignment writes the original object's storage rather than a separate
copy. Checked reference dereference is allowed in safe code. Direct accesses
to `val` must still respect the eventual borrow-lifetime rules.

```flecha
void increment(&i32 value) {
    *value = *value + 1;
}

i32 read_value(&const i32 value) {
    return *value;
}

i32 example() {
    i32 count = 41;
    increment(&count);
    return read_value(&count);
}
```

Dereference syntax is `*value`: its safety depends on the operand's
type. Dereferencing a checked reference is safe; dereferencing a raw pointer
requires an unsafe context. Context selects the permitted borrow at a call
site: `increment` needs an exclusive borrow, and `read_value` needs a shared
borrow. A call-duration borrow ends on return unless an explicit borrowed-result
contract extends it.

Every usable reference must designate a non-null, aligned, initialized, live
object of a compatible type. References have no `none` value and initially
require an initializer. They cannot be freed and do not extend storage lifetime.
A reference to a local object cannot be returned after that object is destroyed.

Shared references may be copied while preserving their restrictions. Mutable
references cannot be copied to produce two independently usable mutable aliases;
passing one reborrows it for the call. The compiler enforces the access and
lifetime rules in section 5. Runtime null checks alone are insufficient.

Raw pointers do not implicitly convert to references. Any explicit unsafe
conversion must establish lifetime, validity, and aliasing obligations for the
whole reference lifetime. Do not add that conversion in the first reference
feature unless those obligations can be represented in its contract.

Assignment to a rebindable reference rebinds it; dereferenced assignment
modifies its target if the target is mutable. The reference variable retains
its declared type:

```flecha
u8 val = 3;
u8 other = 10;
&u8 ref = &val;

ref = &other; // rebinds ref; val is unchanged
*ref = 20;    // writes other
// ref = other; // error: u8 is not &u8
```

Assigning from another mutable reference transfers the handle instead of
creating two independently usable mutable aliases. Rebinding must satisfy
lifetime and access checks for the new target and cannot invalidate outstanding
reborrows of the old handle.

Cross-type rebinding requires a type-system decision rather than an implicit
conversion. The recommended safe rule is to reject rebinding `&u8` directly to
`&u32`: a numeric cast converts a value, not the type or layout of its existing
storage. Declare an `&u32` reference to borrow a u32 object, or explicitly convert
the value into a separate u8 local and borrow that local. In the latter case,
writes affect the converted local, not the original u32.

The suggested `ref = &other as typeof(other)` needs clarification: if `other`
is u32, `typeof(other)` denotes u32, and `typeof(&other)` would denote &u32.
Neither changes the existing &u8 declaration. `typeof` syntax and cross-type
reference reinterpretation are not accepted features of this draft. Any later
reinterpretation mechanism needs a separate unsafe storage-validity contract.

Implicit dereference/member syntax remains open. Borrowed-result annotations
are implemented with the postfix `<- origin` contract.

### 5.2 Const declarations and access qualifiers

Implemented value rule: `const` on a declaration prohibits modifying that
binding's stored value:

```flecha
const i32 limit = 42;
// limit = 43;           // error
// increment(&limit);   // error: cannot mutably borrow const storage
```

A const declaration may use the same trivial-zero initializer omission as a
mutable declaration. Other const types require an explicit initializer.
`const` does not mean compile-time evaluation; runtime expressions may initialize
const bindings.

The agreed fixed-binding aliases are `constptr T` and `constref T`.
Their expanded forms remain supported with identical semantics:

| Idiomatic spelling | Equivalent supported spelling | Meaning |
| --- | --- | --- |
| `constptr u8 ptr` | `const *u8 ptr` | Pointer binding cannot change; target may be written in unsafe code |
| `constref u8 ref` | `const &u8 ref` | Reference binding cannot change; target may be written through the checked reference |

Read-only target access uses the idiomatic forms `*const T` and `&const T`
directly. A leading binding qualifier and a target-access qualifier are
independent:

| Spelling | Rebind the handle? | Modify the target through it? |
| --- | --- | --- |
| `*u8 ptr` | Yes | In unsafe code |
| `*const u8 ptr` | Yes | No |
| `constptr u8 ptr` / `const *u8 ptr` | No | In unsafe code |
| `const *const u8 ptr` | No | No |
| `&u8 ref` | Yes | Yes, subject to exclusive borrowing |
| `&const u8 ref` | Yes | No |
| `constref u8 ref` / `const &u8 ref` | No | Yes, subject to exclusive borrowing |
| `const &const u8 ref` | No | No |

These aliases introduce no conversion or additional runtime representation.
`const u8 value` still denotes an immutable scalar binding. Making a mutable
reference's binding const does not make it a shared reference or permit copying
it into multiple independently usable mutable aliases.

```flecha
u8 value = 3;
u8 other = 10;
constref u8 ref = &value;

*ref = 6;       // allowed: value becomes 6
// ref = &other; // error: fixed reference binding

&const u8 shared = &other;
second := shared; // shared read-only references may be copied
// *shared = 20; // error: read-only target access
```

Reference inference preserves these access rules: `ref := &value` infers `&u8`
for mutable u8 storage, while borrowing a `const u8` binding infers `&const u8`.
Every reference still requires an initializer and compatible storage lifetime.

These qualifiers do not automatically propagate through arbitrary nested
pointers. `const` access alone is not a lifetime or ownership guarantee. A safe
shared reference additionally constrains conflicting mutation through other
safe aliases for the duration of its borrow. No safe operation removes const
access restrictions.

Const declarations and recursive qualifiers are implemented; see
[const declarations and types](const_types.md). User-defined interior mutability,
atomic access, and generic const propagation remain deferred.

## 6. Raw pointers and unsafe operations

The implemented subset, including explicit `&raw value` address creation, is
documented in [unsafe contexts and pointers](unsafe_pointers.md).

Retain raw pointer types `*T`, `*const T`, `*volatile T`, and
`*volatile const T`, and the `const *T` binding qualifier. Prefer `constptr T`
for a fixed pointer binding (`const *T`). Use `*const T` directly for read-only
pointee access, as described above.
Pointee access qualifiers and pointer-binding mutability are independent.

Copying, storing, passing, and equality-comparing initialized raw pointers does not
read their pointees and does not transfer ownership. Taking an address does
not extend the object's lifetime. Raw pointers carry no implicit non-aliasing
guarantee.

Dereferencing, indirect writes, integer-to-pointer conversions, unchecked
pointer casts, and constructing a slice from a raw address require an unsafe
context. Pointer arithmetic should be deferred from the first pointer feature
until its allocation-boundary and provenance rules are specified.

For an access, the programmer must establish appropriate alignment, sufficient
accessible storage, a live compatible object, initialized data when reading,
and permission to read or write. Null and dangling pointers cannot be
accessed. `*const T` cannot be used for writes, even inside `unsafe`; any future
qualifier-removing operation must be explicit and cannot make genuinely
read-only storage writable. `volatile` is neither atomicity nor synchronization.

An `unsafe` block permits explicitly unsafe operations. It does not turn off
type checking, local definite assignment, ownership checking, or ordinary
slice bounds checks. Unchecked indexing, if introduced, must be a separately
named operation. An unsafe function's signature places obligations on callers;
a safe wrapper must enforce those obligations itself.

Unsafe function calls are permitted only inside another unsafe function or an
explicit unsafe block. An unsafe function body establishes an unsafe context.
The standalone prefix-call spelling `unsafe perform(ptr);` is rejected.

```flecha
unsafe void forward(*const u8 ptr) {
    perform(ptr); // permitted; forward inherits the consuming contract
}

i32 caller() {
    unsafe {
        (*u8 ptr, error err) = new<u8>(10);
        if err != none {
            return 1;
        }
        perform(ptr); // permitted; consumes the successful allocation
    }
    return 0;
}
```

A safe wrapper containing an unsafe block must establish every precondition of
its unsafe operations. Merely moving a raw-pointer dereference into a block
inside a safe function does not establish that contract.

## 7. Arrays and slices

Fixed-size arrays have inline contiguous storage and a compile-time length;
constructing one does not implicitly allocate a backing buffer. Fixed arrays now use `[N]T` types, `[a, b]` list initializers, and `[value; N]`
repeat initializers. Implemented rules and limits are documented in
[fixed arrays](fixed_arrays.md). Checked slices are implemented as described in
[checked slices](checked_slices.md).

A slice has a pointer and a `usize` element count. It owns neither the elements
nor their allocation. `[]const T` exposes read-only elements and `[]T` exposes
mutable elements, subject to the borrowing rules. Slicing and subslicing do
not allocate.

Implemented construction from an array or another slice preserves its lifetime
and mutability restrictions. A future safe owning collection must establish the
same borrowing relationship. Slice construction from raw parts is not part of
the 0.1 implementation; any future form must be unsafe and require the caller to
establish the whole range's access contract.

Indices are checked against length. Subranges use half-open bounds with
`0 <= start <= end <= length`; length and byte-offset calculations must not
wrap. Invalid dynamic bounds use a defined runtime failure rather than an
out-of-bounds access. Empty slices may use null storage, but no element access
is valid and no null-pointer arithmetic may be assumed valid.

Slices of owning elements cannot copy those elements out by ordinary indexing;
operations must borrow them until an explicit replacement/removal operation
with sound ownership semantics exists.

`stringv` remains a read-only byte view with string-specific operations. It is
conceptually related to `[]const u8`, but type identity, conversions, and ABI
compatibility must be decided separately. Existing string lengths and methods
are not silently changed by the proposed `usize` slice length.

## 8. Allocation and failure

The implemented `new<T>(value)` and `free(ptr)` built-ins use the process-wide
system allocation service. They replace the earlier `Allocator` handle and
`system_allocator()` proposal. `new<T>` returns `(*T, error)`, supporting both
`(*u8 ptr, error err) = new<u8>(10);` and `ptr, err := new<u8>(10);`.
Custom allocator interfaces remain future work. See [raw_allocation.md](raw_allocation.md)
for the supported types, evaluation rules, and current limits.

The raw API uses typed tuple bindings and explicit failure:

```flecha
unsafe {
    (*u8 ptr, error err) = new<u8>(10);
    if err != none {
        return 1;
    }
    defer free(ptr);
    *ptr = 42;
}
```

`new<T>(value)` allocates one element and initializes it with `value`.
`new<T>(none)` initializes to T's defined zero value, when one exists; it
does not mean uninitialized storage. Success produces a live allocation and
no error; failure assigns null and `AllocationError.OutOfMemory`, without
evaluating the initializer. Fixed arrays use `new<[N]T>(value)`; runtime-sized
scalar buffers use `new<[]T>(count)` and return a move-only `*[]T` owner carrying
`{data, length}`. Their count is evaluated before allocation and their elements
start zeroed. See [dynamic arrays](dynamic_arrays.md). `free(null)` is a no-op.

Safe code uses `new<own []T>(count)` for the same supported element set. Its
`own []T` result is bounds checked, move-only, and automatically released at
scope exit, including when nested in a structure. Slicing it produces a
borrowed `[]T` whose lifetime prevents moving or destroying the owner while the
view is live. The raw `new<[]T>` / `free` form remains available in unsafe code.

Raw allocation and free require an unsafe function or block. Allocations are
not implicitly freed at the end of an unsafe block: allocation lifetime is
independent of unsafe permission. Explicit free, executed deferred free, or an
ownership-consuming function must discharge the allocation's cleanup obligation.
Only matching live system allocations may be passed to `free`; arbitrary stack
addresses and interior pointers are not valid allocation handles.

A pointer may be initialized in one unsafe block and released in a later block.
It can be passed across unsafe function calls. A function that frees the
allocation consumes ownership; an ordinary borrowing function must preserve it.
`*const T` restricts target writes but does not itself encode either contract.

Mandatory compiler analysis now tracks allocation identity, aliases, and possible
states across CFG edges and visible nonrecursive calls. Counting free statements
is insufficient. Leaks, double frees, use-after-free, known invalid frees, null
paths, and unsupported ownership escapes are compile-time errors. Bounded call
interpretation handles local helpers and factories; explicit `own` / `lend`
contracts cover annotated and recursive raw-pointer functions. Recursive calls
that carry only safe values use declared-shape summaries while their bodies are
checked independently. Inferred recursive raw-pointer effects and ownership
contracts for external declarations remain future work. See
[raw_allocation.md](raw_allocation.md) for exact limits. Arbitrary raw-pointer
manipulation and the validity of external input pointers remain programmer
obligations inside unsafe code; complete static memory safety is not promised.

A null-aware operation such as the proposed
`ptr->safe_set(value)` can allocate and initialize when ptr is null,
updating the pointer binding only on success. Otherwise it writes through the
existing pointer. Failure leaves a null binding unchanged. Allocation would use
the system allocation service. This operation still requires unsafe access:
non-null does not establish that a pointer is live, aligned, or writable.
The special pointer-operation syntax and naming remain to be finalized.

### 8.1 Deferred cleanup

The function, callable-value, and member-call implementation is documented in
[defer.md](defer.md). Deferred `free`, bounded static allocation-effect checks,
and `own` / `lend` contracts are implemented. External ownership contracts
remain future work.

A deferred call registers when execution reaches it and executes when its
current enclosing scope exits. It is not hoisted to scope entry. Register
cleanup before work that can exit early:

```flecha
unsafe void perform(own *u8 ptr) {
    defer free(ptr);
    // Work with the still-live allocation.
}
```

The `own` parameter makes the consuming contract explicit. Callers must handle
allocation failure before any use that requires a non-null pointer. The function
must discharge or forward the allocation on every normal exit; tracked caller
aliases are invalid after transfer.

Implemented deferred allocation-cleanup rules:

- Capture the receiver and argument values when registering a deferred call.
  Rebinding the original pointer later does not change the captured allocation.
- Execute registered calls in reverse registration order on normal scope exit,
  return, break, proceed, and jumps out of the scope. A backward jump within the
  same scope does not execute or erase registered calls.
- Account for repeated registrations through loops and jumps; one allocation
  cannot have two pending frees, while distinct allocations need distinct records.
- A scheduled free reserves the cleanup obligation without making the allocation
  dead yet. Explicitly freeing it or passing it to another consumer before the
  deferred free executes is a resource error.
- Type-check unsafe permission where the deferred operation is registered.
  Deferred calls must not return from their enclosing function or redirect its
  control flow.
- Traps and immediate process termination do not execute defers. Registered
  calls run before lexical string and structure destruction. String, view, and
  copyable aggregate captures are independent snapshots destroyed after
  invocation. A noncopyable movable aggregate is moved into the deferred record
  at registration and then transferred to its callee, which destroys it exactly
  once. Structure lifecycle rules are in [structures.md](structures.md).

Defer helps ensure cleanup. It does not prove pointer validity, prevent all
raw-pointer aliasing, or turn an unsafe function into a safe function.

### 8.2 Safe allocation remains separate

Safe references borrow storage; they do not allocate or own it. Safe stack
values and arrays can be borrowed directly. A future safe heap API needs an
owning resource type that establishes reference lifetimes, or an equivalent
checked storage scope. An unsafe raw allocation does not become safe merely
because it is given a reference spelling.

The existing owning string remains a safe-facing value with lexical cleanup
and current copy semantics, subject to its documented implementation limits.
Its allocator/failure policy and any standalone-string move migration are
separate features. A freestanding target should retain stack storage and
checked views without requiring a heap, while defining a bounds-failure
mechanism.

## 9. Control-flow restrictions

No jump may make an uninitialized or moved value readable. Entering a scope
through a position cannot fabricate ownership or a borrow. Leaving a scope
ends its loans and performs the applicable cleanup. A backward jump does not
restore a destroyed owner or erase a live borrow's obligations.

Continue supporting jumps that the analysis can prove valid. Reject transfers
that would create an invalid borrow or require unsupported lifetime state,
with a diagnostic at the transfer or offending access. Do not treat arbitrary
jumps as an exception to the memory rules.

## 10. Decisions and implementation order

Agreed direction:

1. Raw pointers remain available through explicit unsafe operations. Unsafe
   functions are callable only within unsafe functions or unsafe blocks; there
   is no standalone unsafe-call prefix.
2. The system allocator provides initialized raw allocations with explicit
   error results; raw allocation/free require unsafe permission.
3. Allocation obligations can cross unsafe-block and function boundaries.
4. Explicit free and defer provide cleanup; unsafe blocks do not own allocations.
5. Compiler resource diagnostics are mandatory for tracked allocations, without
   claiming arbitrary unsafe code has complete memory-safety or leak guarantees.
6. `constptr T` is the idiomatic spelling of `const *T`; `constref T` is the
   idiomatic spelling of `const &T`. Both expanded forms remain supported.
   Read-only target access uses `*const T` and `&const T` directly.
7. Mutable references use `&T ref = &value`; assignment through `*ref` modifies
   the original value and does not require unsafe permission for a checked reference.
8. Assigning to a rebindable reference rebinds it; assigning through its
   dereference writes the referent if mutable. Mutable reference handles cannot
   be implicitly duplicated; shared read-only reference handles may be copied.
9. Rebinding preserves the declared reference type. No casts between reference
   types are introduced; a different target type needs a matching declaration.

Next proposals to settle:

1. More general inferred resource effects beyond explicit single-source,
   grouped-source, and `<- ...args` result-origin contracts.
2. Reference borrowing of strings/views and nested reference storage.
3. Ownership contracts for external declarations and more general inferred
   recursive effects.
4. Partial aggregate moves and dynamic-array relocation of move-only elements.
5. Any migration of existing string copies to moves. No such migration is
   agreed or implemented by this document.

The detailed dependency order and acceptance criteria are maintained in
[memory_implementation_plan.md](memory_implementation_plan.md). The first
implementation milestone, const declarations and compound type representation,
is documented in [const declarations and types](const_types.md). Unsafe pointer operations are documented in [unsafe pointers](unsafe_pointers.md).
Checked scalar and pointer-slot references are documented in
[checked references](checked_references.md). Checked slices, system allocation,
direct, indirect, and member deferred calls, explicit cross-function ownership
contracts, general whole-value moves, and single- and multi-origin
borrowed-result contracts are also implemented. More general inferred resource
effects remain future work. Named and generic
[error values](error_values.md) are implemented.
[Tuples and destructuring](tuples.md) are implemented.

Concurrency, atomics, shared ownership, partial moves, and arbitrary pointer
arithmetic remain outside the first safe-reference feature.

## Specification alignment

`SPEC.md` identifies the implemented reference, slice, allocation, ownership,
and cleanup subsets and links to their focused contracts. Its design goal of
locally visible cost still has a known exception: existing string assignment,
owning parameter passing, conversions, and deferred string/view snapshots can
allocate implicitly. A future standalone-string move proposal must preserve or
deliberately revise that shipped behavior.
