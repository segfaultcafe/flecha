# Memory safety invariants

Status: normative contract for the implemented safe language subset. The
concurrency invariant is a release gate for future safe concurrency and is not
yet an implemented Flecha guarantee.

This document states the properties that every accepted use of Flecha's safe
language surface must preserve. Feature guides describe syntax and individual
mechanisms; this document defines the common safety contract those mechanisms
must satisfy together.

The contract applies both to programs containing no `unsafe` operations and to
safe callers of abstractions whose implementations contain `unsafe`. An
`unsafe` block changes who must justify an operation; it does not weaken the
properties observable through a safe interface.

This is a fundamental Flecha rule:

> An `unsafe` block transfers a proof obligation from the compiler to the
> programmer. It does not make memory safety optional.

The compiler's guarantee is therefore conditional at an unsafe abstraction
boundary: safe code preserves these invariants provided that every unsafe
implementation it relies on satisfies its documented preconditions,
postconditions, and retained obligations. An incorrect unsafe implementation
can invalidate assumptions made by otherwise safe callers. Such a failure is a
soundness defect in that unsafe implementation or its contract, not permission
for safe code to weaken these invariants.

This document does not claim that arbitrary raw-pointer or foreign code is
memory-safe. It also does not yet define a concurrent execution or atomic
memory model. Those limits are part of the contract, not implicit exceptions
to it.

## Core terms

The following concepts are distinct even when an implementation represents
them with the same machine address:

- A **storage place** is a region or position capable of holding a value. It
  has an extent, alignment, and storage duration. A place may successively hold
  different objects.
- An **object lifetime** is the interval between the valid construction and
  destruction, movement, or replacement of one particular object in a storage
  place. Constructing another value in the same place begins a new object
  lifetime and does not continue the identity of the previous object.
- A **loan** is a temporary permission to access a particular live object or a
  tracked region of that object under shared or exclusive access rules. A loan
  records its possible origin objects and projections; it is not merely a
  machine address.

An object's **identity** distinguishes one object lifetime from every earlier
or later object occupying the same storage place. An address may help locate
an object, but address equality alone does not establish object identity,
liveness, provenance, or permission to access it.

## 1. Storage and object-state validity

Every tracked storage place and object lifetime is in a state compatible with
each operation performed on it. The relevant simplified object states are:

```text
uninitialized -> live -> moved
                      -> destroyed
```

Construction starts a new object lifetime and makes that object live. Reading,
borrowing, moving, replacing, or destroying it requires the state appropriate
to that operation. A move ends access through the source object identity until
a new object is constructed there. Destruction or replacement ends the old
object lifetime. Safe code cannot read an uninitialized place or access an
object whose lifetime has ended.

Constructing a new object in a previously moved, destroyed, or replaced place
creates a new identity. Existing references and loans to the previous object
do not become valid for the new object merely because its address, type, or
representation is the same.

Control-flow joins and loops must preserve this property on every reachable
path. When a resource is live on only some paths, generated cleanup must use
the runtime state needed to destroy it only on those paths.

## 2. Unique cleanup obligation

Every live resource managed by Flecha has exactly one active cleanup
obligation. Copying is permitted only when it creates an independent value
with its own valid cleanup behavior. Moving a resource transfers its existing
obligation rather than duplicating it.

Every normal exit from a scope must discharge each remaining obligation
exactly once. Cleanup occurs in the specified reverse order. A moved source is
not cleaned up; the destination that received its obligation is. Raw pointers
and checked references do not acquire cleanup obligations merely by pointing
to or borrowing storage.

Resource-producing failure paths, early returns, jumps, deferred calls,
aggregate construction, and nested destruction must preserve the same rule.

## 3. Object-lifetime and loan validity

A checked reference, checked slice, borrowed string view, or borrowing callable
cannot outlive the object or tracked object region from which it was derived.
While such a loan is live, every possible origin required by the loan must
preserve its object identity, remain within its lifetime, and retain storage
valid for the loan's projection.

Moving, replacing, destroying, reallocating, or otherwise relocating an owner
is rejected when that action would end or change an origin object required by
a live loan. Reconstructing an object at the same address does not preserve the
old loan. Address-sensitive values are immovable after construction and are
constructed directly in their final storage, but address stability alone is
not proof of object-lifetime or loan validity.

Returned loans must be tied to declared input origins. A return cannot derive
a safe loan from a local, temporary, unrelated parameter, or untracked raw
pointer. The compiler substitutes the caller's actual origins for the callee's
formal origins and retains the resulting loan until its last possible use.

## 4. Access exclusivity

A live mutable loan grants exclusive access to its borrowed region. No
overlapping read, write, move, replacement, destruction, or competing loan may
occur through another path while that exclusivity is required.

Multiple shared read-only loans may coexist. While any shared loan is live, the
borrowed region cannot be mutated through a safe alias. Reborrowing suspends
conflicting use of the parent handle until the child loan ends.

Overlap is defined over storage paths. Distinct structure fields may be proven
disjoint, while access to a whole aggregate overlaps all of its fields. Where
the compiler cannot prove regions disjoint, including currently supported
array and slice index cases, it must conservatively treat them as overlapping.
Rejecting a safe program is permitted; accepting a program that violates this
invariant is not.

## 5. Interprocedural preservation

Calling a function must preserve the preceding invariants without inspecting
the callee body at every call site. A callable's semantic signature therefore
includes its ownership effects and the possible origins of every borrowed
result.

For a borrowed result, `<- source` or a grouped source contract is verified
against every reachable return. At a call, the compiler substitutes actual
arguments for those sources and propagates the resulting loans into the
caller's state. The same summary must survive methods, modules, generic
specialization, recursion, callable values, and indirect calls.

The current language requires every function that returns a borrowed value to
declare this contract. Call sites do not repeat it. A wrapper that returns the
loan across another function boundary must expose its own verified contract;
this is part of its API rather than an implementation annotation.

Inference of contracts for private helpers may be considered later, provided
the compiler materializes and verifies the same summary. Public or separately
compiled interfaces must not depend on whole-program body inference.

## 6. Unsafe encapsulation

An unsafe operation must establish all preconditions required by the raw
operation and preserve every active safe-language obligation. In particular,
unsafe code must not:

- dereference an invalid, misaligned, or insufficiently live raw pointer;
- create a checked loan without a live origin object, valid projection, and
  justified lifetime;
- mutate, move, free, or relocate storage in conflict with a live safe loan;
- treat a newly constructed object as the continuation of an earlier object
  merely because it occupies the same address;
- duplicate or lose a cleanup obligation;
- expose a safe function whose permitted callers can trigger undefined
  behavior; or
- claim thread-safety without satisfying the accepted concurrency model.

Unsafe code may encapsulate operations the compiler cannot prove, but its safe
interface must be sound for every input admitted by that interface. The
compiler checks safe callers under the premise that this obligation is met; it
cannot in general prove the premise from an arbitrary unsafe implementation.
Safety comments and tests are evidence for this obligation; neither replaces
it.

## Concurrency safety gate

Safe concurrency is not enabled merely by ownership or exclusive mutable
borrowing. Before Flecha exposes safe thread creation, scoped threads, atomics,
mutexes, channels, or equivalent primitives, a concurrency specification must
define and enforce at least:

1. the concurrent executions that constitute a data race and whether a data
   race is undefined behavior;
2. which value shapes may be moved to another thread and which may be shared
   between threads;
3. the lifetime rules for owned and borrowed thread captures, including the
   distinction between detached and scoped threads;
4. the synchronization operations that establish ordering and
   happens-before relationships;
5. the supported atomic operations, memory orders, and interaction between
   atomic and non-atomic access;
6. the rules for interior mutability, raw pointers, operating-system handles,
   callbacks, and foreign code; and
7. how compiler transformations and the runtime preserve the specified model.

The required concurrency invariant is:

> A program using only safe operations cannot cause a data race under Flecha's
> specified concurrent memory model.

This invariant is **pending**, because that memory model is not yet specified.
The current enforcement boundary is the absence of a general safe Flecha
thread-spawn surface. Host functions documented as safe for concurrent calls
make narrow library guarantees; they do not constitute a complete language
memory model.

Concurrency may remain outside a release without blocking unrelated safe
single-threaded work. It must not enter the safe surface until this section is
replaced by an accepted normative model and corresponding compiler, runtime,
and test requirements.

## Derived operation rules

The invariants imply the following rules for common invalidating operations:

| Operation | Required safety effect |
| --- | --- |
| Move | Transfer the value, borrow origins, and cleanup obligation; invalidate the source; reject conflicting live loans. |
| Replacement | Evaluate the replacement without losing the old value; reject conflicting loans; destroy the old value once; install the new live value. |
| Destruction | Require a live cleanup obligation and no invalidated loan; run typed cleanup exactly once; mark the place destroyed. |
| Reconstruction in place | End the old object lifetime and begin a distinct one; do not transfer old loans solely because the address is reused. |
| Whole-aggregate move | Transfer every contained obligation and borrow origin together. |
| Field access | Check overlap against the selected field path and every enclosing aggregate. |
| Reallocation | Require that no live loan depends on the old address, unless a future operation has an explicit stronger contract. |
| Unsafe raw access | Preserve all tracked loans and cleanup obligations in addition to satisfying raw-pointer validity. |

Partial aggregate moves remain unsupported until the compiler can represent
the per-field states and cleanup obligations needed to preserve these rules.

## Verification requirements

A memory-affecting language or library feature is complete only when its design
identifies which invariants it relies on and its implementation has focused
positive and negative coverage. As applicable, tests must cover:

- every normal scope exit and control-flow join;
- use after move and double movement;
- early return and failure after partial progress;
- nested and reverse-order destruction;
- destruction followed by reconstruction at the same address;
- movement, replacement, destruction, and reallocation under live loans;
- shared/mutable and parent/child borrow conflicts;
- borrowed-result propagation through direct, indirect, generic, recursive,
  method, and cross-module calls;
- invalid local, temporary, raw, and wrong-parameter borrow escapes;
- allocation failure without leaks, double cleanup, or stale handles; and
- for future concurrency, data-race rejection and memory-order litmus tests.

Compiler diagnostics must reject an unsupported or unproven safety effect
before lowering. `unsafe` permission must not silently disable the compiler's
tracked ownership, initialization, movement, or borrowing checks.

## Release policy

This document is required now because Flecha already has checked references,
borrowed results, whole-value moves, owning aggregates, automatic teardown, and
safe abstractions implemented with internal unsafe code. It should be reviewed
whenever one of those mechanisms changes.

For v0.3.0:

- the implemented single-threaded invariants above are a documentation and
  regression gate;
- every v0.3 memory-affecting feature must state how it preserves them; and
- concurrency may remain a stretch goal and may be omitted without weakening
  the single-threaded safety contract.

Before any general concurrency primitive is exposed as safe, the pending
concurrency invariant is a hard design and implementation gate. Shipping an
unsafe or experimental host bridge does not satisfy that gate and must not be
presented as safe Flecha concurrency.

## Related specifications

- [Memory model and remaining proposals](memory_model.md)
- [Checked stack references](checked_references.md)
- [Checked slices](checked_slices.md)
- [Reference and slice returns](ref_and_slice_returns.md)
- [Ownership contracts](ownership_contracts.md)
- [Structures](structures.md)
- [Raw allocation](raw_allocation.md)
- [Dynamic arrays](dynamic_arrays.md)
- [Definite assignment](definite_assignment.md)

