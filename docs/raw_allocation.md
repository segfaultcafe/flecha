# Raw allocation: new and free

`new<T>(value)` and `free(pointer)` use the process-wide system allocation
service. Both require an unsafe function or an explicit unsafe block. There is
no public allocator handle and no automatic free at the end of an unsafe block.

```flecha
unsafe {
    (*u8 pointer, error err) = new<u8>(42);
    if err != none { return 1; }
    defer free(pointer);
    *pointer = 43;
}
```

Inferred destructuring is `pointer, err := new<u8>(42);`. The result is a tuple
`(*u8, error)`, so `pointer := new<u8>(42);` binds the entire tuple, not just the
pointer. The existing mixed form `(pointer, error err) = new<u8>(42);` also works
when pointer has already been declared.

Runtime-sized scalar buffers are also available as `new<[]T>(count)`, returning
`(*[]T, error)`. The move-only `*[]T` owner carries both data and length. The
count is evaluated before allocation, and all elements start at zero. See
[dynamic arrays](dynamic_arrays.md) for indexing, transfer rules, supported
types, and the first Flecha standard-library container.

## Allocation and evaluation

The compiler computes T's native size and alignment. The runtime allocates
zeroed storage with that alignment. A successful allocation then evaluates the
initializer exactly once and constructs T directly in that storage. This
preserves the addresses of immovable structures, including fresh factory
results and structures nested in arrays or tuples.

Success returns a non-null pointer and `none`. Failure returns null and the
built-in `AllocationError.OutOfMemory` value converted to generic `error`.
Allocation failure **does not evaluate the initializer**, call constructors, or
run teardown. Assignments and reference-handle transfers inside the initializer
are therefore conditional; definite-assignment and borrow checking account for
the failure path. The checker does not yet correlate `err == none` with those
initializer side effects, so initialize needed locals before allocation.

```flecha
unsafe {
    (*Box box, error err) = new<Box>(Box::init(42));
    if err == AllocationError.OutOfMemory { return 1; }
    defer free(box);
    box->setY(10);
}
```

`AllocationError` is a built-in named error family with the single failure
variant `OutOfMemory`; `none` means success as usual. The names `new`, `free`, and
`AllocationError` are declared by the compiler. `new<T>` is built-in typed syntax,
not general user-defined generics.

The error reports failure to obtain the outer allocation. Failures inside an
initializer retain their existing behavior: for example, owning-string runtime
allocation failure terminates the process, and bounds traps do not unwind.
There is no constructor-exception or partial-construction protocol.

For `new<[]T>`, success packages `{data, count}` in the owning buffer; failure
packages `{null, 0}`. A successful zero-count allocation remains non-null and
releasable. An explicitly expected legacy `*T` may receive the data pointer,
consuming the buffer owner and dropping its visible count.

## Supported values

Allocatable types are sized owned values: scalars, enums, errors, raw pointers,
owning strings, structures, supported fixed arrays, and tuples. Const-qualified
values are allowed and produce pointers with read-only targets. References,
slices, `stringv`, void, and otherwise unsupported compound types are rejected.

`new<T>(none)` performs defined default initialization where T supports
`none`. Enums use their declared default variant. Structures use their generated
or overridden `T::zero()`; a structure without valid default construction is
rejected. Raw pointer elements are
initialized to null by `none`; they do not allocate their targets.

Existing transfer rules apply to an initializer naming an existing value:
copyable fields are copied, while a noncopyable movable structure, tuple, or
fixed array moves into the new allocation after allocation succeeds. The source
then becomes unavailable until reassigned. Because allocation failure skips the
initializer, the source remains live on that path. Unsafe-tear values are
immovable and must be constructed directly in their final allocation storage.
Zero-sized values receive a distinct live allocation that must still be freed.

## Release and deferred release

`free(pointer)` evaluates its pointer argument once. For a non-null pointer it
invokes the structure's tear, destroys owned fields/elements in reverse order,
then releases the backing storage. Direct calls to tear remain forbidden. A
pointer to const T may be freed; target constness does not remove the cleanup
obligation. `free(null)` and `free(none)` do nothing. Void pointers cannot be
freed through this typed API because teardown needs the pointee type.

Free does not reset the caller's variable. The compiler marks tracked aliases
as freed. Free must receive null or the exact live pointer returned by a
matching new allocation, with its original pointee type (optionally const).
Stack addresses, interior pointers, foreign allocations, and already-freed
pointers violate this unsafe contract. Releasing a raw-pointer field or element
does not recursively free the object it points to; only owned values are torn
down.

`defer free(pointer);` captures the pointer value at registration and runs on
scope exit using the existing defer rules. It does not consume the allocation
immediately, and rebinding the original variable does not change the captured
pointer. `defer new<T>(...)` is rejected because it would discard the allocation.
An ordinary ignored allocation result is rejected as a possible leak.

## Mandatory allocation analysis

After type, definite-assignment, and borrow checking succeed, the compiler runs
allocation analysis before lowering or object emission. It is always enabled;
`unsafe` does not disable it. Violations and unsupported ownership effects are
errors, not optional warnings.

The checker follows allocation identities through resolved local bindings,
raw-pointer aliases, tuple destructuring, immutable aggregate snapshots, and
assignments through references to pointer bindings. It checks reachable CFG
paths through branches, loops, `also`, `break`, `proceed`, and `jumpto`.
Success and failure of `new` remain separate paths: failure produces null and
`AllocationError.OutOfMemory`, and skips the initializer. Error/pointer equality
checks and short-circuit boolean expressions retain this relationship.

It diagnoses:

- Discarding an allocation, overwriting its last tracked handle, or leaving a
  scope/function with an outstanding cleanup obligation.
- Freeing a tracked allocation twice, including through aliases or deferred calls.
- Using a tracked freed pointer, or dereferencing a pointer known to be null on
  an allocation-failure path.
- Freeing known stack storage or an interior address rather than the allocation.

Each lifetime diagnostic identifies the operation or exit and includes an origin
note at the allocation. Diagnostics describe a possible path; the checker does
not prove arbitrary numeric or relational conditions infeasible.

Deferred arguments are snapshots. Their calls execute in reverse registration
order on each scope exit, including return and non-local loop control. A pending
free keeps its allocation obligation alive; it does not free the pointer early.
A later explicit free conflicts with that pending cleanup.

Visible, nonrecursive helper bodies are interpreted with the caller's tracked
arguments. This supports borrowing, conditional consumption, deferred consumers,
reference-mediated pointer reassignment, and factories returning pointers or
`(*T, error)`. Returning a live allocation transfers the obligation to the caller;
returning it after explicit or deferred free is rejected. Calls and returns do
not implicitly move or null raw-pointer bindings.

```flecha
unsafe void consume(*u8 pointer) {
    defer free(pointer);
}

unsafe void example() {
    pointer, err := new<u8>(42);
    if err != none { return; }
    alias := pointer;
    consume(pointer);
    free(alias); // FLECHA-S0049: the allocation was already freed.
}
```

### Conservative limits and the unsafe boundary

Explicit `own` and `lend` contracts and owning fields are described in
[ownership contracts](ownership_contracts.md). Unannotated helpers retain the
behavior above.

Ownership that cannot be represented is rejected with `FLECHA-S0052`. Current
examples include storing a tracked allocation in an ordinary non-owning structure field or another
heap object, mutable/indexed addresses and slices over containers of allocation
handles, dynamic indexing of those handles, unannotated recursive pointer effects, and
revisiting an allocation site while its earlier instance remains live. This
last restriction also applies to multiple outstanding results from one factory
allocation site. Use a per-iteration scope with `defer free(pointer)` when
allocating in a loop. Previously freed aliases remain stale if the site repeats.

Analysis is bounded at 128 distinct states per CFG node and 100,000 processed
states per root function, including interpreted callees. Exceeding either budget
is an error, not a silently successful check. Complex but valid programs may
need simpler control flow. Declared ownership contracts support recursive calls; general inferred recursive
summaries and owning container storage remain future work.

This is an allocation checker, not a proof that arbitrary unsafe code is memory
safe. Raw pointer parameters entering a root function are symbolic external
pointers: their provenance, validity, nullability, and possible aliasing remain
caller obligations. Known arguments at visible call sites are checked, but
unknown external/field-derived pointers and stack-address lifetimes do not gain
a safety guarantee. Free with unverified provenance is rejected. The analysis
tracks allocation obligations, not byte usage or peak heap size; automatic
strings and structure values continue to use their existing cleanup rules.
General safe heap-owner primitives remain future work. The first concrete
dynamic collection is [`RawArray<T>`](dynamic_arrays.md), implemented in
Flecha using these raw contracts. The same document describes `Array<T>`, its
safe public wrapper; both container values now use whole-value moves, while
move-only element relocation remains unsupported.

## Diagnostics and verification

`FLECHA-S0047` diagnoses unsupported allocation/free types and invalid built-in
usage. Existing argument, unsafe-context, const, copy, definite-assignment, and
borrow diagnostics continue to apply.

| Code | Meaning |
| --- | --- |
| FLECHA-S0048 | Possible leak / lost allocation |
| FLECHA-S0049 | Possible double free |
| FLECHA-S0050 | Possible use after free |
| FLECHA-S0051 | Invalid free of known non-allocation storage |
| FLECHA-S0052 | Ownership cannot be verified by the current checker |
| FLECHA-S0053 | Null dereference on a tracked path |

`SemanticMemoryTests` cover aliases, failure paths, cleanup ordering, control
flow, helper effects, factories, stale loop aliases, and unsupported escapes.
`CompilerDiagnostics.MemoryOwnership` verifies that rejected code produces no
object file and that lifetime diagnostics include allocation origins.

`NativeSemanticTests.RawAllocation` covers scalar/string/structure/array/tuple
allocation, const and zero-sized objects, stable construction addresses,
destruction order, null free, and deferred release on control-flow exits. Its
native harness uses linker wrapping to inject one allocator failure without a
production runtime test hook. Runtime tests check zeroing, alignment, overflow,
and invalid alignment. ASan/UBSan and native leak checks cover this fixture.
See [runtime_abi.md](runtime_abi.md) for the storage ABI and
[../examples/raw_allocation.fl](../examples/raw_allocation.fl) for a runnable
example returning 42.
