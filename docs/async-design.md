# Async design foundation

**Implementation in progress; async programs are not executable yet.** This
summarizes the selected foundation of [IFG 2026-0007](IFG/IFG_2026_0007.txt).
The IFG remains Draft while low-level future APIs and remaining failure details
are under review. Checked result destructuring is implemented in the gated frontend.
Async is not a v0.4 release requirement.

## Selected language operations

| Syntax | Meaning |
| --- | --- |
| `async` | Modifies a named function that can suspend. Its written return type is its completed value type. |
| `.await` | Awaits an async call or joins a scoped child. Pending work suspends the current task. |
| `shoot` or `>>-` | Creates and queues a child owned by the current lexical block. Both spellings have identical semantics. |

A task is not an OS thread. All tasks in the initial model share one executor
thread. An already-ready await need not suspend. Blocking calls remain blocking.

An ordinary async call must be immediately awaited, except when directly
spawned. Direct awaiting executes within the caller's task; it does not queue
an independent child. Recursive async calls are required: rejecting recursion
because an embedded frame would have unbounded size is not the selected policy.

`shoot` and contiguous `>>-` are prefixes at operand positions. Existing
`x >> -y` and `x>>-y` remain shift followed by unary minus. Existing member names
such as the synchronous `Command::spawn` remain valid. `.await` is postfix.
Initially spawning requires a direct async call. Fallible child creation must
be destructured and checked before joining its handle; the earlier
`(shoot f()).await` shorthand is superseded by this requirement.

The approved synchronous entry is a generated wrapper for `async main` only.
Async blocks, indirect async calls, async interface dispatch, and async extern
functions are outside the initial support boundary and must be diagnosed.
Ordinary synchronous `defer` and teardown remain valid inside async functions;
suspending during deferred calls or teardown is unsupported.

## Recoverable allocation

Frame allocation is recoverable. Allocation alone must not force every async
function to declare the crashable effect. A completed value and execution
failure are distinct outcomes: failure before the body runs constructs no
value of the function's written return type. This distinction also applies to
recursive calls that fail to create a deeper frame after earlier frames have
already started.

The selected surface uses immediate, checked destructuring:

```flecha
async i32 calculate() { return 42; }

async i32 main() {
    child, createError := shoot calculate();
    if createError != none { return 1; }

    value, awaitError := child.await;
    if awaitError != none { return 1; }
    return value;
}
```

Direct awaiting uses `value, err := calculate().await`; the symbolic creation
form is `child, err := >>- calculate()`. These examples show the approved
contract, not executable compiler support yet.

On failure the value binding is uninitialized. The compiler must reject access
until the corresponding error is proven to be `none`. Initially, fallible
creation and await results require immediate destructuring. This is not an
ordinary always-initialized `(T, error)` tuple: no dummy reference or owned value
is constructed. An ordinary error returned by the function remains part of T,
and is not silently flattened into the execution-failure slot. No public
`AsyncResult<T>` wrapper or `.value`/`.error` API is selected.

Child creation is recoverable too, superseding the earlier crash-on-allocation
policy. Neither frame creation nor scheduler admission should require a
crashable declaration solely for allocation. The selected keyword is `shoot`,
replacing `spawn`; `>>-` remains its exact synonym. `spawn` is an ordinary
identifier again, including in existing synchronous process APIs.

The frontend uses an internal `CheckedAsyncResult` semantic kind, displayed as
`checked-async<T>` in expression type records. It is not a tuple or a public
wrapper. Immediate destructuring currently requires two fresh, named bindings
with `:=`; discards, reused destinations, storing, forwarding, returning, and
nested creation/await shorthand are diagnosed. The AST retains its existing
destructure, spawn, and await nodes; the semantic model links each success-only
binding to its execution-error binding. Editor hover shows that relationship.

CFG success/failure paths preserve the relationship through `== none`,
`!= none`, inverted conditions, short-circuit conditions, early exits, nested
scopes, and loop backedges. Definite assignment rejects a value read on any
remaining failure path. Ordinary assignment to the error, taking its address,
or capturing it invalidates the proof relationship; copying an error does not
forward the proof. These conservative rules prevent an unrelated `none` from
making an absent value accessible.

Creation failure does not move direct arguments or publish a child/capture
loan. Successful creation transfers ownership and retains captures until join
or drain. Await consumes a child on both execution outcomes and only initializes
its value on success. Execution failure cannot restore arguments already passed
to a running computation. Borrowed aggregate results preserve their origins
through both checked and subsequent ordinary destructuring. Runtime tests use
non-default-constructible, move-only completions containing ordinary errors to
check that failure leaves result storage untouched and cleans up exactly once.

Allocation no longer adds a crashable effect or changes callee lookup. Explicit
crashable callees retain the ordinary `!` call/effect rules. Void await completion
and normal scope exits with potentially unobserved child execution failures
receive explicit unsupported diagnostics. Early-exit cancellation/drain planning
is preserved. Failure forwarding and argument-evaluation timing remain unresolved;
this frontend analysis does not select an executable evaluation schedule.
Calls, assignments, increments, construction, closures, or ownership moves nested
inside argument preparation must first be bound in a separate statement. Simple
prepared bindings, literals, and borrow/projection expressions remain accepted.
This restriction also applies to the receiver of a directly awaited/spawned
method; it avoids selecting the order of argument effects and fallible frame
allocation while that policy remains open.

Root allocation failure has a separate approved policy: the generated entry
wrapper reports a diagnostic and returns a nonzero process status before user
code executes or argument ownership transfers.

The private runtime reserves scheduler and stable frame storage before
transferring arguments or publishing a task. Failure leaves ownership with the
caller. Initialization transfers already prepared arguments without executing
the async body. Public argument-evaluation rules still need specification alongside the
selected checked-destructuring contract. Failure reporting and retirement
must work without a further successful allocation.

## Lexical scope ownership and cleanup

Every lexical block inside an async function owns the children spawned directly
within it. Discarding a handle never detaches its child.

- Normal fallthrough joins the block's children.
- `return`, or `break`/`proceed` leaving the block, requests cancellation of its
  unfinished children and drains them.
- Exiting multiple blocks applies cleanup from innermost to outermost.
- Child cleanup finishes before captured locals are destroyed. Existing
  synchronous deferred calls and local cleanup retain their valid ordering.
- A transfer staying within a block does not clean up that block. Cross-scope
  `jumpto` is initially rejected inside async functions.
- A child result is consumed once. Already-joined children need no additional
  waiting; unconsumed completed results are destroyed at scope cleanup.

Cancellation is cooperative. It is observed before first execution and at
await/scope-drain boundaries. A pending operation must acknowledge safe
retirement before its frame or captured storage can be destroyed. An early
exit can wait indefinitely for a child that does not reach a cancellation point.

An ordinary error value alone neither cancels siblings nor propagates
implicitly. Returning an error follows the same early-exit cleanup rules as
any other return. Public handling of an unobserved execution failure during
implicit scope joining remains to be specified with the result API.

## Scheduling and low-level interoperability

Shoot queues work without executing it inline. Runnable tasks use FIFO order,
duplicate notifications coalesce, and callbacks only enqueue work. Polling
cannot reenter the executor. Pending operations are polled again after a
notification; they are not busy-polled.

The approved intermediate protocol is private to the C++ compiler/runtime.
It distinguishes pending, ready, cancelled, and allocation-failed computations.
Every terminal outcome requires retirement of callbacks and operations that
could access frame storage. A cancellation request alone is not retirement.
Stale wake handles cannot address a released task or a new task reusing storage.

Handwritten C++ awaitables currently exercise this protocol with synthetic
events and recursive frame creation. Compiler-generated state machines must
use the same protocol. A public Flecha future, poll, and wakeup API, including
high-/low-level interoperability, remains required work; the private prototype
is not completion of that requirement.

Notifications may represent application events, timers, I/O, or another task.
Foreign-thread notifications must be marshalled to the owning executor thread.
Channels belong in the standard library over this general machinery; channels,
network stacks, worker pools, atomics, and multithreading are not prerequisites.

## Ownership across suspension

- Suspended tasks retain their live borrows. Shared read-only borrows may
  coexist; overlapping mutable access remains prohibited.
- Owners cannot move, be destroyed, or be invalidated while a task or pending
  operation uses them.
- Joining releases capture loans only when no returned value or outstanding
  operation retains them. Borrowed results keep their origins after completion.
- Requesting cancellation never releases loans. Retirement and cleanup must
  finish first.
- Compiler-managed stable storage permits borrowing locals across suspension
  without a public Pin-like mechanism.
- `unsafe` preserves checked-borrow rules and cannot make synchronous functions
  suspend. Raw-pointer backends must preserve lifetimes, aliasing, stable
  addresses, and callback retirement through completion and cancellation.

The [concurrency safety gate](memory_safety_invariants.md#concurrency-safety-gate)
remains in force. This foundation enables no shared-memory concurrency model.

## Implementation status

| Area | Current status |
| --- | --- |
| Parser and AST | Named async functions, postfix await, `shoot`/`>>-`, AST traversal. |
| Semantic records | Async callable effect, scoped task type, explicit checked-result kind and value/error binding records; immediate destructuring and unsupported-use diagnostics. |
| Scope analysis | Success/failure CFG paths and conditional definite assignment; inner-to-outer join/cancel-and-drain plans, unsupported implicit-join failure and cross-scope jump diagnostics. |
| Borrow analysis | Outcome-sensitive captures and argument moves, explicit join release, aggregate borrowed-result origins, and consumed-handle checks. Generated-frame/suspension validation remains. |
| Private runtime | FIFO executor, wake coalescing, cooperative retirement, stable frames, transactional creation, and separate allocation failure outcome. |
| Resumable code generation | Not implemented. Async declarations issue `FLECHA-S0089` and cannot reach synchronous lowering. |
| Public Flecha futures/results | Checked destructuring implemented in the frontend; public low-level future API and executable integration remain. |
| Tooling | VS Code and Neovim syntax highlighting; checked expression types, success-condition hover, and binding navigation/occurrence tests. |

Tests cover ready and delayed synthetic events, registration-time notifications,
stale callbacks, reentrancy, cancellation, allocation failure injection, result
consumption, checked-result control flow, move-only ownership, borrowed aggregates,
and borrowing/scope rules. These checks do not establish
that generated Flecha async programs execute correctly. The execution gate must
remain until lowering, cleanup, failure handling, and interoperability are
validated together.

With a testing build configured, run the focused checks with
`ctest --test-dir <build-directory> -R 'AsyncFrontendTests|AsyncRuntimeTests' --output-on-failure`.
The runtime tests use injected allocation failures and handwritten recursive
frames. Generated state-machine coverage is still required before enabling
async execution.
