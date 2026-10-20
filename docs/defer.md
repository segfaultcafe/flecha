# Deferred calls

`defer callee(arguments);` captures a function, callable value, or structure
method call now and executes it when the current lexical scope exits. See
[the runnable example](../examples/defer.fl).

```flecha
void report(i32 value) { stdoutln(value); }
i32 main() {
    i32 value = 10;
    defer report(value);  // captures 10
    value = 20;
    {
        defer report(1);
        defer report(2);
    }                    // prints 2, then 1
    return 42;           // prints 10 before returning 42
}
```

## Capture and execution

- A callable value or method receiver evaluates first, followed by arguments
  once from left to right. Their side effects happen immediately; the outer
  call runs later. A direct function target needs no runtime evaluation.
- A deferred callable value moves into the deferred record, so its closure
  environment remains alive and the source callable is unavailable afterward.
  The environment is destroyed exactly once after invocation and after argument
  snapshots are released.
- An instance method captures its receiver according to the declared receiver
  parameter before evaluating explicit arguments. A checked receiver loan stays
  active until scope exit; raw receiver handles retain their normal unsafe
  restrictions.
- Scalar and raw-pointer arguments capture their current values. Copyable fixed
  arrays, tuples, and structures capture independent snapshots. Reassigning the
  original variable does not change such a snapshot.
- Both `string` and `stringv` arguments get independent owned byte snapshots.
  This copy can allocate. A `stringv` parameter receives a view of its snapshot
  during the deferred call. All snapshots are destroyed after that call.
- Copyable tuple arguments recursively copy owning string elements; discarded
  tuple results recursively release their owned storage.
- A noncopyable movable structure, tuple, or fixed array is moved into the
  deferred record when the statement registers. The source is unavailable
  afterward until reassigned. At scope exit that ownership transfers to the
  by-value callee, which tears it down; the record does not destroy it a second
  time.
- References and slices capture borrowed handles, without copying their targets.
  Their loans remain active until scope exit, including the lifetime of any
  parent reborrow. They must not outlive their owners.
- Each scope executes its registrations in reverse registration order. Inner
  scopes finish before outer scopes. Calls run before that scope destroys its
  owned strings, structures, and compiler temporaries.
- Registration happens only if the statement executes. The same statement can
  register multiple calls, each with independent captures.
- Non-void results are discarded and their owned storage is destroyed.
- Copyable structure arguments (including arrays and tuples containing them)
  receive independent copies of owning fields. Structure results construct
  directly in final storage before cleanup, including immovable results. See
  [structures](structures.md) for lifecycle rules.

Direct functions, callable values and resolved structure or enum methods are
supported, including `stdout` and `stdoutln`. Parenthesizing the call is
allowed. Deferred blocks and built-in member operations such as `slice.len()`
are not supported. `defer` does not require unsafe permission by itself; an
unsafe callee, receiver, or argument operation still requires an enclosing
unsafe block or function at the registration site.

## Scope exits and control flow

Calls run on ordinary fallthrough, `return`, `break`, `proceed`, and `jumpto`
edges that leave their scope. A return expression evaluates before cleanup, so
cleanup cannot change the already computed return value. A counted loop's body
cleans up before its update executes, including on `proceed`.

A jump within the same scope keeps all pending registrations. A backward jump
can register another instance; none are overwritten. Jumps entering a scope
initialize its cleanup state even when they skip its opening brace. Skipped
`defer` statements do not register. Existing rules still restrict positions and
jumps to loops.

An `also` clause follows existing lexical-scope rules: the owning body remains
active through its `also` clauses, and a clause has its own nested scope.

Traps and immediate process termination do not unwind scopes. If argument
evaluation traps, that call is not registered. If a deferred callee traps,
remaining calls do not execute. Flecha does not introduce exception handling.

## Checked borrows

```flecha
void set(&i32 target) { *target = 42; }
i32 main() {
    i32 value = 0;
    { defer set(&value); }
    return value;        // 42; the deferred borrow has ended
}
```

An exclusive deferred borrow prevents conflicting reads, writes, moves, and
rebindings until its scope exits. A shared deferred borrow permits reads but
prevents mutation. This initial analysis conservatively rejects two pending
exclusive borrows of the same owner, even though their calls would execute
sequentially. Deferred calls do not count as having already initialized or
modified their targets.

Raw pointers retain their existing unsafe contract. `defer` alone does not
prove arbitrary pointer validity. The mandatory allocation analysis does track
known system allocations, pending deferred frees, double frees, leaks, and
use-after-free paths; its contracts and conservative limits are documented in
[raw allocation](raw_allocation.md) and
[ownership contracts](ownership_contracts.md).

## Implementation and validation

Each active scope has a linked stack of captured call records, dispatched by
call site. Registrations allocate records on the function stack, and scope exit
restores the stack after draining them. Repeated registration within one active
scope uses space proportional to its pending calls; unbounded registration can
exhaust the stack. The queue itself requires no heap allocator.

The shared CFG records argument evaluation at registration. Synthetic capture
symbols retain borrowed origins until the matching scope-exit edge. LLVM cleanup
blocks handle all exits, including jumps into sibling scopes.

Unit tests cover parsing, diagnostics, borrow lifetimes, lowering, and verified
LLVM IR. Native tests cover capture order, strings/views, arrays, deferred return
values, all exit forms, traps, backward jumps, and repeated scope entry with
large captures. The sanitizer target includes native defer tests and leak checks
for the runtime's string allocations; generated Flecha object code itself is
not sanitizer-instrumented.
