# Async design foundation

**Implementation in progress; public async execution remains gated.** An
internal test-only generator can execute direct awaits, scheduled scoped
children, joins, and lexical child cleanup, but the CLI, synchronous lowering
path, and `async main` entry remain disabled. This
summarizes the selected foundation of [IFG 2026-0007](IFG/IFG_2026_0007.txt).
The IFG remains Draft while low-level future APIs and remaining interoperability details
are under review. Checked result destructuring, void await, and explicit failure forwarding are
implemented in the gated frontend.
Async is not a v0.4 release requirement.

## Selected language operations

| Syntax | Meaning |
| --- | --- |
| `async` | Modifies a named function that can suspend. Its written return type is its completed value type. |
| `.await` | Awaits an async call or joins a scoped child. Pending work suspends the current task. |
| `.await?` | Awaits and forwards execution failure after scope cleanup; success yields the completed value, or no value for `void`. |
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
form is `child, err := >>- calculate()`. Direct awaits and scoped child
scheduling are executable only through the internal test entry described below.

On failure the value binding is uninitialized. The compiler must reject access
until the corresponding error is proven to be `none`. Fallible creation requires immediate checked destructuring. Non-void await
supports either checked destructuring or explicit `.await?` forwarding. This is not an
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
wrapper. Checked two-result destructuring currently requires two fresh, named bindings
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
crashable callees retain the ordinary `!` call/effect rules.

### Void completion and explicit failure forwarding

A void await produces only an execution error: `err := operation().await;` or
`err := child.await;`. `none` means success. There is no completed-value binding
or dummy `void` value.

The postfix `.await?` forwards execution failure as the enclosing async
function's execution failure. On success its type is the callee's completed
`T`, including ordinary errors inside `T`; it does not flatten or forward them.
For example:

```flecha
async i32 calculate() {
    value := operation().await?;
    return value;
}

async void finish() {
    operationReturningVoid().await?;
}
```

`child.await?;` is also valid for a void child. Void forwarding is a standalone
statement with no binding or returned value. Failure exits cancel and drain
owned children from inner to outer scope, run deferred calls and guarded
cleanup, then complete with the original execution error. They construct no
completed value. A forwarded failure is distinct from returning an ordinary
error as a successful completed value. Joining consumes a child on either
outcome.

This frontend chunk supports `.await?` as a whole initializer, destructuring
value, return value, or expression statement, allowing parentheses around the
whole operation. Nested suspension inside a larger expression is diagnosed
until temporary storage and evaluation sequencing are lowered. `?` is not a
general error operator and does not apply to `shoot` or ordinary calls.
Ordinary returned errors use prefix `try call()` instead; see
[error propagation](error_values.md#explicit-propagation-with-try). `try` requires
a compatible final error field in the enclosing completed return type, while
`.await?` forwards execution failure independently of that type. Ordinary `try`
returns take the normal return cleanup path, storing the completed error result
before cancellation/draining; they do not enter the failed terminal state.
`AwaitResult` records preserve completed types, destinations, and forwarding;
frame plans have a separate saved execution error and failed terminal state.

Normal scope exits must not leave potentially unobserved child execution
failures: explicitly await the child and inspect its error or use `.await?`.
Such unobserved exits are rejected, rather than implicitly forwarding failures.
Early exits retain cancellation/drain semantics.

### Argument evaluation ordering

Frame storage and scheduler admission resources must be reserved before
argument evaluation, including method receiver preparation. If reservation
fails, argument expressions do not run, no argument ownership transfers, and
no task handle is published. Once reservation succeeds, argument effects are
not rolled back if execution later fails. The internal generator implements
this ordering for direct awaits and both child-creation spellings: it reserves
indirect frame storage and, for children, the lexical record plus scheduler
admission before evaluating each source argument exactly once into an argument
pack. Only then does it initialize the frame and queue the child. Reservation
failure therefore executes no argument effects and transfers no ownership.

Root allocation failure has a separate approved policy: the generated entry
wrapper reports a diagnostic and returns a nonzero process status before user
code executes or argument ownership transfers.

The private runtime separates reservation from initialization. It reserves
scheduler and stable frame storage before transferring arguments or publishing
a task. Failure leaves ownership with the caller. Initialization transfers the
generated argument pack without executing the async body. Failure reporting and
retirement work without a further successful allocation.

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
any other return. Normal scope exit with a potentially unobserved child execution failure is
rejected; explicitly await it. `.await?` observes and forwards execution failure
through the same early-exit cleanup sequence.

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

## Frame and suspension plans

After semantic and borrow checking succeeds (apart from the execution gate),
`SemanticModel::AsyncPlanFor` exposes a verified `AsyncFramePlan` for each async
function. Units with other errors do not publish plans. `DumpAsyncFramePlan`
provides deterministic scope, slot, continuation, liveness, and initialization
records for compiler tests and debugging. This is an internal analysis API;
there is no new source syntax or public runtime ABI. The plans feed the
test-only generated-frame path described below.

The plan preserves the specialized CFG's separate checked success and failure
continuations. Operation records distinguish child creation, direct await, and
child join; retain resolved callees and prepared operands; and identify the
value, execution error, and consumed child bindings. The completed type is
unchanged, including ordinary errors inside aggregates. Pending resumes the
same guarded operation. Direct computations use indirect stable storage, so
recursive calls do not imply recursively embedded frame layouts.

Frame slots are logical storage requirements, not target byte offsets. This
first implementation deliberately gives all parameters and locals stable,
guarded storage. Backward read liveness is recorded separately from lifetime
retention: every active lexical slot is retained across suspension, including
owners whose only remaining users are borrowed views, child captures, deferred
calls, or destruction. No slot reuse or early frame-storage elimination is
performed. This conservative policy retains transitive borrow origins without
inventing a second borrow checker. Existing borrow validation still determines
whether those lifetimes and accesses are legal.

Initialization records distinguish absent, initialized, and moved alternatives.
A cleanup guard must use the actual runtime state, not treat a static "possibly
initialized" result as a value. Operation and task-scope records likewise need
runtime activation guards. The CFG outcome events describe analysis facts,
not an executable argument-evaluation schedule. Generated direct-await and
child-creation bindings therefore implement allocation before argument
evaluation, transactional transfer, and guarded polling explicitly instead of
replaying those events.

Scope exits expand into inner-to-outer cancellation requests (for early exits),
draining, synchronous deferred calls, and guarded owner destruction. A drain
can reach cleanup only after retirement; cancellation alone never clears
storage. Return values have separate storage through draining. Deferred-call
site records include the whole saved call, including scalar and owned argument
temporaries; each scope also has a dynamic registration stack so repeated visits
to a site preserve every call and its order. Its physical storage/allocation
strategy is deferred to executable lowering. As in synchronous code, deferred
calls run before reverse-order owner destruction in each scope.

The verifier checks continuation targets, checked outcomes, frame references,
initialization/read consistency, suspension retention, and retirement-before-
cleanup. It also checks correspondence to the validated CFG and storage plan.
This establishes an inspectable input for subsequent lowering, not correctness
of generated state machines. General expression temporaries that cannot span a
supported suspension remain ordinary expression-lowering work; no hidden async argument effects or nested expression suspension are enabled.
Forwarded failure preserves its error through cleanup without reading the
function's completed-value storage. Void awaits allocate no completed-value slot.

## Internal resumable control lowering

`LowerAsyncMachine` accepts only a verified plan published by semantic analysis,
with no errors other than the async execution gate. It lowers continuations to
an immutable internal machine and synthesizes cancellation paths for the active
lexical scopes. `DumpAsyncMachine` exposes the control instructions for tests.
This API is separate from the synchronous lowerer and has no command-line switch.

`GenerateAsyncMachine` emits LLVM initialization and poll functions. The generated
native control code saves its continuation on Pending, resumes the same operation,
and caches terminal results. It tracks actual initialization and operation
activation with frame-owned guard arrays; static may-initialization masks are
not cleanup guards. Reservation precedes argument initialization. Argument move
guards change when initialization transfers ownership, before polling can suspend,
and not on reservation failure. Postorder analysis events do not evaluate async
operands again. A completed value is published only on Ready.

Cancellation before first execution skips the body. Cancellation at an await
first retires the active operation, then cancels/drains lexical children and
runs defers and guarded destruction from inner to outer scope. A pending drain
cannot reach destruction. Cancellation during a return's drain discards the saved
owned return value only after retirement. Inactive task scopes and defer stacks
are skipped. Backend callbacks must be nonthrowing and must not allocate while
reporting failure or retiring storage; invalid outcome encodings trap as an
internal protocol error.

`GenerateAsyncValuesForTesting` adds a deliberately internal source-to-native
binding for direct awaits and scoped children. It uses LLVM `DataLayout` to
assign target-correct byte offsets and ABI alignment for control state,
initialization and operation guards, parameters, locals, temporaries, saved
completed values, saved execution errors, and indirect `Computation` objects.
Values that may survive suspension have stable frame addresses. Direct
operations remain indirect, so recursive frames have finite layouts and stable
addresses.

The generated callbacks evaluate ordinary values, assignments, branches, loops,
returns, and the synchronous helper bodies reached by supported async code.
They reserve, initialize, queue, poll/resume, join, transfer, retire, and release
direct and scheduled child frames. Per-scope dynamic child records preserve
multiple live children from repeated visits to one source site. Normal cleanup
drains; early exits cancel then drain from inner to outer before guarded local
destruction. Checked await, void await, and `.await?` keep execution failure
separate from an ordinary returned error, including aggregate completed values.
Flow bindings consume postorder analysis events without re-evaluating source
expressions. Generated cleanup uses runtime initialization guards and reverse
ownership order; saved returns and pending cancellation are destroyed exactly
once, only after retirement.

This entry is not wired to `NativeCompiler`, the CLI, or synchronous lowering.
It still rejects dynamic async defer storage with an explicit error. Public
future APIs, timers/I/O, async interface/behavior
dispatch, and production `async main` integration remain later work. Unsupported
value or continuation shapes fail generation rather than silently omitting their
effects or cleanup.

`AsyncLoweringTests` JIT-compile and execute the generated control functions with
explicit test backend bindings. They cover pending/ready/failure paths, both shoot
spellings, void completion, ordinary-error completion, ordinary `try` returns,
loop reuse, ownership guards, nested cancellation, saved returns, real private
executor wakeups, recursive computation frames, and injected frame-allocation
failure. `AsyncValueLoweringTests` start from Flecha source and execute generated
frames. They cover immediate and pending completion, repeated wakes, recursion,
branches, loops, early returns, checked and forwarded failure, void and aggregate
results, ordinary `try`, stable borrows, move-only transfers, both creation
spellings, FIFO child execution, checked/forwarded joins, repeated source-site
children, lexical cancel/drain, reservation failure at every child boundary,
partial success, cancellation, stale wakes, and exactly-once destruction. Test code
only supplies synthetic readiness/allocation failure and observations; source
evaluation, transfers, and cleanup are generated. Neither suite removes
`FLECHA-S0089` or the synchronous lowering gate.

## Implementation status

| Area | Current status |
| --- | --- |
| Parser and AST | Named async functions, postfix await, `shoot`/`>>-`, AST traversal. |
| Semantic records | Async callable effect, scoped task type, explicit checked-result kind, value/error bindings, and void/forwarding await records; unsupported-use diagnostics. |
| Scope analysis | Success/failure CFG paths and conditional definite assignment; inner-to-outer join/cancel-and-drain plans, forwarded-failure exits, unobserved child-failure and cross-scope jump diagnostics. |
| Borrow analysis | Outcome-sensitive captures and argument moves, explicit join release, aggregate borrowed-result origins, and consumed-handle checks. Generated-frame/suspension validation remains. |
| Private runtime | FIFO executor, wake coalescing, cooperative retirement, stable frames, transactional direct/child creation, dynamic lexical child records, join watching, cancel/drain, and separate allocation failure outcome. |
| Frame/suspension planning | Verified internal plans, conservative stable slots, initialization/move alternatives, read liveness, and suspendable scope-exit continuations, saved execution errors and distinct failed terminals; deterministic dumps and corruption tests. |
| Resumable code generation | Internal machine lowering plus target-layout, value/operation, direct-await, child scheduling/joining, lexical child cleanup, and guarded owner-cleanup bindings with source-to-generated-frame execution tests. Dynamic defer storage, production entry integration, and the public ABI remain. Async declarations still issue `FLECHA-S0089` and cannot reach synchronous lowering. |
| Ordinary returned-error propagation | Prefix `try` implemented through CFG, borrow/allocation analysis, synchronous lowering and native code generation; supports error-only and multiple success fields. |
| Public Flecha futures/results | Checked destructuring, void await and `.await?` implemented in the frontend; public low-level future API and executable integration remain. |
| Tooling | VS Code and Neovim syntax highlighting; checked expression types, success-condition and failure-forwarding hover, and binding navigation/occurrence tests. |

Tests cover ready and delayed synthetic events, registration-time notifications,
stale callbacks, reentrancy, cancellation, allocation failure injection, result
consumption, checked-result control flow, move-only ownership, borrowed aggregates,
borrowing/scope rules, and internal generated direct-await/scoped-child frames.
The execution gate remains until the later defer, entry, public ABI, and
interoperability work is validated together.

With a testing build configured, run the focused checks with
`ctest --test-dir <build-directory> -R 'AsyncFrontendTests|AsyncRuntimeTests|AsyncPlanTests|AsyncLoweringTests|AsyncValueLoweringTests' --output-on-failure`.
Runtime and generated-frame tests use injected allocation failures and recursive
frames. Public end-to-end entry and unsupported-feature coverage remain required
before enabling async execution.
