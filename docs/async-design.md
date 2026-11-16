# Async design foundation

**Design only; not implemented.** This guide summarizes the selected foundation
of [IFG 2026-0007](IFG/IFG_2026_0007.txt). The IFG remains Draft because runtime,
lifetime, and API decisions are still open. Preparing these documents before
v0.4 does not make async implementation a v0.4 release requirement.

## Three language operations

| Syntax | Meaning |
| --- | --- |
| `async` | Marks a function that can suspend. Its declared return type describes the completed result. |
| `.await` | Waits for completion. If pending, suspends this task so other runnable tasks can progress. |
| `spawn` or `>>-` | Creates and schedules a scoped child task, returning a handle. The two spellings have identical semantics. |

**A task is not an OS thread.** All tasks in the initial model share one
executor thread. An already-ready await need not suspend. Async does not make
blocking code nonblocking or CPU work parallel.

The following examples show selected syntax, not programs the current compiler
can run. Exact task-scope boundaries and creation-failure behavior remain open.

```flecha
async i32 calculate() {
    return 42;
}

async i32 sequential() {
    first := calculate().await;
    second := calculate().await;
    return first + second;
}

async i32 concurrent() {
    first := spawn calculate();
    second := >>- calculate();

    a := first.await;
    b := second.await;
    return a + b;
}
```

`sequential` awaits each computation in turn within the current task.
`concurrent` schedules both children before joining either. The computation in
this example is trivial and gains no speed; the distinction matters when work
can suspend. Scheduling a child does not guarantee immediate execution.

For the initial high-level surface, async calls require an async function and
must be awaited or explicitly spawned:

```flecha
async void usage() {
    calculate().await;           // Complete the operation; discard its result.
    child := spawn calculate(); // Transfer child lifetime responsibility to its scope.
    child.await;                // Join the child; discard its result.
    calculate();                // Error: unawaited async call.
}
```

No detached execution is implied by discarding a result or a task handle.
Handle consumption, unobserved errors, and precise scope cleanup remain design
questions. `async { ... }` and entry from synchronous code are not settled.

## One model, two levels

Application code uses `async`, `.await`, and scoped task creation. Low-level
code should also be able to manage futures and polling explicitly. Both levels
must interoperate through one contract and preserve the same lifetime rules.

The compiler handles suspension and ownership checks; the executor schedules
tasks; libraries define operations. A library-provided awaitable should work
with `.await` without adding operation-specific compiler support.

A possible low-level contract is `poll(context) -> Ready(result) | Pending`.
These are explanatory names, not selected public APIs. Polling advances work
without blocking. If it returns pending, a notification can arrange another
attempt. The executor does not repeatedly poll waiting operations in a busy loop.

```text
Running task -> operation pending -> task waiting, state retained
                                          |
                              completion/readiness notification
                                          |
                                    task runnable
                                          |
                               executor resumes the task
```

Notifications can come from another task, an application event, a timer, an
external callback, or an I/O backend. The model is general; sockets are one
validation workload. A long computation can cooperate by yielding between
steps. An arbitrary blocking function still needs adaptation or execution
elsewhere; a worker-thread bridge is outside the first design.

Channels should be standard-library types implementing queues, capacity,
endpoint closure, and waiting through the same general machinery. A channel
carries values; a task handle represents completion of a particular child.
Receiving a value does not prove its producer has finished. Channel API names,
error behavior, and allocation policy are not selected yet.

## Ownership and decisions still needed

Suspension keeps a task's live values and borrows alive. Other tasks cannot use
suspension as permission for conflicting access. A scope must keep captured
storage valid until its children and outstanding operations have safely retired.
Low-level futures must obey these constraints too.

The proposed exit policy is to join children on normal fallthrough and request
cancellation and drain them on early exit. **This policy is still under review.**
An error value alone is not proposed to trigger cancellation. Cooperative
cancellation cannot guarantee progress from a task that never yields.

Before implementation, the IFG must settle:

- Task-scope boundaries, synchronous executor entry, and any async block form.
- Cancellation observation points, result consumption, and child-error handling.
- Future/wakeup APIs and deliberate low-level access to unawaited work.
- Frame storage, allocation failure, stable addresses, and borrow lifetimes.
- Scheduling, reentrancy, and the full grammar for `spawn`, `>>-`, and `.await`.

The existing [concurrency safety gate](memory_safety_invariants.md#concurrency-safety-gate)
remains in force. This foundation does not enable threads, atomics, or a shared
memory concurrency model. Implementation should begin with a deterministic
executor and synthetic completion events, then validate real timers and I/O.
