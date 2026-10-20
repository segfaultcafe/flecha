# Memory feature implementation history and remaining work

Status: const types, unsafe pointers, checked references, fixed arrays, and
iteration, checked slices, deferred calls, and tuples/destructuring are merged.
Named and generic error values are merged; see [error_values.md](error_values.md).
Structures, raw allocation, mandatory allocation identity diagnostics, bounded
visible-call analysis, explicit `own` / `lend` contracts, and initial owning-field
support are merged in `master`; see [structures](structures.md),
[raw allocation](raw_allocation.md), and
[ownership contracts](ownership_contracts.md).

Based on the agreed rules in [memory_model.md](memory_model.md), with the
const-type milestone merged into `master` at `c9e6ee4`. The numbered sections
preserve the historical implementation order and acceptance criteria; branch
names are historical context, not active work. Remaining proposals are labeled
explicitly.

## Constraints carried into every milestone

- `constptr T` means `const *T`; `constref T` means `const &T`.
- `*const T` and `&const T` are the idiomatic read-only target forms.
- `constval` is not a language feature.
- Unsafe functions can be called only in unsafe functions or unsafe blocks.
  The standalone `unsafe perform(ptr);` spelling is rejected.
- Raw pointer access, allocation, and free require unsafe permission. Unsafe
  blocks do not automatically free allocations.
- Reference creation uses `&value`, with explicit declarations or `:=`
  inference. References require initialized storage, cannot be null, and do
  not own or extend the lifetime of their targets.
- Reference assignment rebinds a compatible handle; dereferenced assignment
  writes its target. Fixed bindings cannot be rebound. No cross-type reference
  casts are introduced.
- Mutable references transfer rather than duplicate their handles; shared
  references may be copied. Borrowing calls can temporarily reborrow.
- Preserve current string copy semantics and the requirement for initialized
  `stringv` declarations. General owner moves and delayed view initialization
  are separate changes.
- Preserve the existing reachable CFG definite-assignment behavior. Raw-pointer
  diagnostics do not constitute a guarantee about arbitrary unsafe code.

## 1. Const declarations and compound type representation

Implementation branch: `feat/const-types`. Depends on: current master.

Implemented behavior and limits: [const declarations and types](const_types.md).

Deliver a complete first feature for const scalar/value declarations, and the
structural type foundation required by later pointer/reference features.

- Extend syntax types beyond the current primitive-only `TypeNode` and semantic
  types beyond a flat enum. Represent recursive pointer/reference targets and
  qualifiers without losing exact source ranges.
- Distinguish a binding's writability from its target's access qualification.
  Preserve qualified nested target types in equality, diagnostics, function
  parameter types, and bound IR.
- Add lexical/parser support for `constptr` and `constref`, normalized to their
  expanded forms. Do not expose reference operations before milestone 3.
- Implement const declarations and assignment diagnostics using resolved symbols,
  including shadowing and function parameters where applicable.
- Require a const declaration initializer for this milestone. Do not silently
  introduce delayed const initialization or compile-time evaluation.
- Reconcile SPEC section 8.1 with implemented definite assignment. Document
  which compound syntax is represented but not yet usable end to end.

Acceptance: runtime-initialized const values generate verified native code;
reassignment reports the exact offending target; alias and expanded type forms
normalize identically; unsupported compound operations fail with diagnostics.
All existing programs and tests retain their behavior.

## 2. Unsafe contexts, raw pointer operations, and writable expressions

Implementation branch: `feat/unsafe-pointers`. Depends on: 1.

Implemented behavior and limits: [unsafe contexts and pointers](unsafe_pointers.md).

- Parse unsafe function modifiers and explicit unsafe blocks. Enforce permission
  lexically during semantic analysis; a safe callee does not inherit permission
  merely because its caller is unsafe.
- Add nested block handling to every relevant walker: scope binding, position
  collection, CFG construction, lowering, cleanup, and LLVM generation.
- Support raw pointer declarations, null/none initialization, assignments,
  parameters, results, and equality with null. Preserve target constness and
  fixed pointer bindings. No raw pointer allocation is needed yet.
- Replace name-only assignment targets with a bound representation of writable
  storage locations. Lower direct and indirect loads/stores using the same
  type and mutability checks. Distinguish reading a pointer value from reading
  its pointee in dataflow analysis.
- Make dereference and indirect stores unsafe operations. Writing through
  `*const T` remains an error inside unsafe code.
- Keep reference-producing `&value` separate from explicit raw address creation
  using `&raw value`. Do not introduce an implicit reference-to-pointer cast.
- Defer pointer arithmetic, integer-to-pointer casts, volatile operations, and
  arbitrary reinterpretation until their own contracts are specified.

Acceptance: an unsafe pointer function called from a C/native test harness can
read/write a valid supplied object. Tests reject safe-context dereferences and
unsafe calls, const-target writes, and rebinding a const pointer. This does not
need an allocator or a new public foreign-function feature.

## 3. Checked stack references and reusable flow analysis

Implementation branch: `feat/checked-references`. Depends on: 1 and 2.

Implemented behavior and conservative limits: [checked references](checked_references.md).
This stage supports scalar and raw-pointer targets; string/view borrowing and
nested reference storage require additional lifetime contracts.

- Separate reusable graph construction/control-flow edges from the current
  definite-assignment transfer logic. Retain its tests and use the same edges
  for borrow and moved-handle analysis.
- Implement `&T`, `&const T`, `constref T`, their expanded forms, `&value`, safe
  dereference, and reference inference. Include `&*T` references to raw-pointer
  variables: the first dereference accesses the pointer slot safely; accessing
  the raw pointee still requires unsafe permission.
- Implement same-type rebinding, mutable handle transfer, shared handle copy,
  and call-duration reborrowing. Reject reads of moved handles until reassigned.
- Enforce conservative borrow lifetimes and conflicting-access rules from the
  first usable reference implementation. Track possible origins across joins,
  loops, and jumps; do not ship unchecked references under a safe type spelling.
- Start with initialized local/parameter storage and non-escaping borrowing
  parameters. Reject unsupported reference returns, stored escapes, and raw
  pointer conversions until explicit contracts are available.
- Reject moving from a fixed mutable-reference binding initially unless a
  precise policy has been chosen; its handle must never become implicitly
  shareable. Rebinding cannot invalidate an outstanding child reborrow.
- Add contextual integer-literal typing sufficient for the agreed example
  `u8 val = 3; ref := &val; *ref = *ref * 2;`, with representability diagnostics.
  Do not introduce implicit conversions between already typed integer values.
  Contextual shared borrowing must infer read-only access from const storage.

Acceptance: the mutation example produces 6; reference-to-pointer nesting,
fixed bindings, shared copies, mutable moves, and ordinary reborrowing calls
work. Null/uninitialized references, incompatible target types, dangling
references, and conflicting safe borrows are rejected with exact ranges.
The first milestone may conservatively reject otherwise valid lifetime patterns.

## 4. Defer and scope-exit execution

Implementation branch: `feat/defer`. Depends on: 2 and 3.

Implemented behavior and limits: [deferred calls](defer.md). Direct functions,
callable values, and resolved structure or enum methods are supported.

- Add `defer call(...);`, initially limited to statically resolved calls. Define
  argument capture at registration, reverse registration order, and
  failure/termination behavior before implementing lowering.
- Capture arguments exactly once when reached. Registration is
  conditional on execution, not on lexical presence in a block.
- Execute registrations on normal exit, return, break, proceed, and outward
  jumps. Inner scopes run first. A jump within the same scope retains its
  registrations; repeated registration must preserve each captured instance.
- Extend cleanup infrastructure for dynamic registrations where cycles require
  them. A list of source statements or one flag per defer site is insufficient
  for repeated registration in one active scope.
- Preserve captured values and their borrow obligations until execution.
  Define captured owning-string cleanup and ordering relative to existing
  lexical destruction before enabling those argument types. Diagnose any
  unsupported capture instead of generating a dangling borrow.
- Type-check unsafe permission at the defer statement. Resolve deferred calls
  into the flow/resource model; do not count them as already executed frees.

Acceptance: native observable calls prove execution order, value capture,
conditional registration, and every non-local exit. Backward jumps with multiple
registrations run each one once. Existing string cleanup passes sanitizers.
Allocation is not needed to test this feature.

## 5. Tuple results, error values, and typed destructuring

Tuple implementation branch: `feat/tuples`. Depends on: 1 and 2; based on the
committed defer implementation to cover aggregate cleanup and deferred captures.

Tuples, multiple results, and typed/mixed destructuring are implemented; see
[tuples.md](tuples.md). Named and generic error values are merged; see
[error_values.md](error_values.md). Typed `new<T>` calls are implemented in the
allocation milestone.

This is a prerequisite for the agreed allocator API, not an allocator-specific
parser shortcut.

- Implement the minimum tuple return/type representation and a defined native
  calling convention, plus the error-value type and no-error value needed by
  allocation. Specify error representation before LLVM/runtime integration.
- Support inferred bindings, fully typed bindings, and the agreed mixed form
  `(ptr, error err) = ...`, where ptr already exists and err is declared.
- Evaluate the right-hand side once before destination writes. Specify order,
  duplicate-target handling, scope visibility, and invalid/const destinations.
- Preserve initialization and reference/resource effects across tuple elements.
- Add generic call syntax sufficient for the typed compiler built-in
  `new<T>(...)`; full user-defined generics are not required.

Acceptance: ordinary functions return/destructure multiple values correctly
in native tests, including the mixed declaration/assignment form. Incorrect
arity/types and writes to const bindings have precise diagnostics.

## Structures (implemented before allocation)

Implementation branch: `feat/structures`. Depends on the merged error-value
milestone. See [structures.md](structures.md) for the complete implementation
contract, restrictions, and native coverage. Includes nominal fields and
privacy, `Type::method` declarations, `.`/`->` access, generated/overridden init,
`build`, automatic tear, recursive cleanup, deep copies of owning fields, and
direct final-storage construction for immovable results. General moves and
stored-borrow lifetimes remain separate work.

## 6. System allocation and intraprocedural resource diagnostics

Implementation branch: `feat/raw-allocation`. Depends on: 2, 4, 5, and structures.

The built-ins, typed destruction, native failure injection, and sanitizer coverage
are implemented, together with mandatory CFG resource identities, local aliases,
leak/double-free/use-after-free errors, null-path checks, and deferred cleanup.
See [raw_allocation.md](raw_allocation.md) for supported paths and conservative
rejections. This is not a guarantee for arbitrary unsafe input pointers.

- Use the system allocation service through compiler built-ins; no public
  `Allocator` handle or `system_allocator()` is planned.
- Implement unsafe `new<T>(value)` returning `(*T, error)` and `free(ptr)`, with explicit result/error
  values. Allocation failure must set the pointer to null; successful allocation
  initializes one object. `none` means defined zero initialization.
- Initially restrict element types to those whose size, alignment,
  initialization, and release semantics are implemented. Do not accept owning
  elements or borrowed references without their construction/destruction rules.
- Define the runtime ABI for size, alignment, failure, and deallocation metadata.
  `free(null)` is a no-op. Do not assume
  malloc's default alignment supports every future type.
- Extend the CFG analysis with allocation identities, pointer aliases, pending
  cleanup, and freed/escaped states. Track live resources separately from
  pointer-variable initialization. A null result is initialized but not usable.
- Diagnose established local double frees, use-after-free, loss of the last
  tracked allocation handle, and outstanding cleanup obligations on exits.
  Treat a scheduled free as a reserved cleanup obligation, not an immediate free.
- Make analysis outcomes explicit when raw escapes or aliasing are unknown;
  choose diagnostic severity without claiming complete unsafe-code verification.
  Repeated allocations at one source site must not make stale aliases appear
  to refer to the newest allocation automatically.

Acceptance: allocation failure injection, correct alignment, manual free,
deferred free, cross-block allocation/free, and loop/jump cleanup have native
coverage. ASan/UBSan and leak checks exercise the runtime. Diagnostics distinguish
null, uninitialized, possibly freed, and outstanding allocations where provable.

## 7. Ownership effects across function calls

Proposed branch for the remaining work: `feat/resource-call-effects`. Depends on: 6.

Bounded interpretation of visible nonrecursive callees is already implemented
in `feat/raw-allocation`, including consuming helpers, conditional consumption,
returned allocations, deferred cleanup, and caller aliases. Explicit contracts now support independent checking and annotated recursion.
Owning fields support build-time initialization and verified tear cleanup. General
inferred recursive summaries, external functions, and field replacement remain
future work. The historical inferred-summary proposal below is not the current
implementation contract.

- Infer borrow/consume/escape summaries for visible functions. Include deferred
  effects on all returning paths and compute summaries to a fixed point for
  recursion. Choose how conditional consumption is represented or rejected.
- Apply consuming effects to every tracked alias in the caller. A pending defer
  cannot coexist with another consumer of the same allocation.
- Define explicit contracts for external or otherwise unavailable callees;
  unknown effects cannot silently receive a proven-safe summary.
- Preserve independent checks for nullability, pointer-variable initialization,
  and allocation lifetime. A function may require a successful allocation even
  if it reliably schedules cleanup.

Acceptance: the agreed allocating caller plus unsafe `perform` with deferred
free succeeds. Tests catch caller reuse/double-free, conditional consumption,
forwarded ownership, and conflicting deferred cleanup. Safety claims remain
limited by the documented unsafe boundary.

## 8. Null-aware pointer initialization helper

Proposed branch: `feat/pointer-safe-set`. Depends on: 6 and 7.

- Implement the proposed `ptr->safe_set(value)` spelling, defaulting to the
  system allocator. Finalize its special pointer-slot grammar before coding.
- Evaluate the pointer location and value once. Allocate/initialize/update the
  slot only on null success; otherwise write the live target. Failure preserves
  the original null pointer and returns an error.
- Require unsafe permission, a writable pointer binding, writable target type,
  and consistent allocation/resource effects. Fixed pointer bindings and
  read-only targets must not bypass their restrictions through this operation.
- Do not interpret a null check as a lifetime or provenance check. Document what
  the operation guarantees despite its name.

Acceptance: null success/failure and non-null updates work through ordinary
pointer slots and supported reference-to-pointer slots; const violations fail.

## 9. Inline arrays and checked slices

Fixed-array behavior is documented in [fixed arrays](fixed_arrays.md), and
checked slice behavior in [checked slices](checked_slices.md). Both are
implemented. This stage was moved ahead of defer at the user's request.
Fixed-array length syntax is `[N]T`; slices use `[]T` and `[]const T`.

Slice implementation branch: `feat/checked-slices`. Depends on: 1, 2, and 3.

- Finalize fixed-array syntax and whole-value initialization. Implement contiguous
  inline layout without implicit heap allocation.
- Add `[]T` and `[]const T`, array-to-slice construction, indexing, and subslicing
  without implicit allocation. Use usize lengths and explicit bounds/overflow
  checks. Define empty-slice behavior.
- Reuse reference origin, lifetime, access, and move rules. Initially reject
  partial initialization, partial moves, and unsupported overlapping mutable
  ranges rather than weakening safe access.
- Defer raw-parts-to-safe-slice construction until its unsafe contract is defined.
  No existing string ABI or length type changes implicitly.

Acceptance: native read/write and subslice examples, bounds failures, empty
ranges, borrow escapes, mutability violations, and cleanup of supported element
types are tested. This milestone can move earlier after 3 if arrays become the
priority; it does not depend on the system allocator.

## Later, separately scoped features

- Safe heap owners and allocator-aware collections that can supply references.
- More precise multi-source and heap-owner lifetime contracts. Single-source
  borrowed reference/slice results and immediately destructured flat
  multi-results are implemented by `feat/ref-and-slice-returns`; see
  [reference and slice returns](ref_and_slice_returns.md).
- Flow-aware stringv origins, then delayed view initialization if sound.
- Any explicit migration of existing strings to move semantics.
- Custom allocators, aggregate partial initialization/moves, pointer arithmetic,
  volatile access, concurrency, and shared ownership.

These are not silently bundled into the first pointer/reference feature.

## Validation and delivery

At each feature milestone, add focused positive and negative tests across the
layers it changes, exact diagnostic ranges, verified LLVM IR, and native
behavior where executable. Reuse tests when refactoring the CFG; do not rebuild
definite assignment as a separate lexical analysis.

Before completing each implementation branch, run the applicable suite:

- `make build` and `make test`.
- `make test-semantic` for semantic/dataflow changes.
- `make native-definite-assignment` to preserve the existing result of 42.
- `make test-sanitizers`, with native allocation/leak regressions as they arrive.
- `make test-neovim`, updating syntax support with new language tokens.
- `git diff --check`; keep generated artifacts out of commits.

The checked-in examples use supported syntax. Focused fixtures under
`src/tests/fixtures` remain the acceptance coverage for individual memory
features and diagnostics.

Growable owning arrays and explicit iterator handles remain future container
work.
