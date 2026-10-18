# Flecha runtime ABI

Generated native objects call a versioned C ABI implemented by the Flecha
runtime archive. The current ABI version is `1`, and every exported symbol is
prefixed with `flecha_runtime_v1_`.

The public declarations live in `src/include/runtime/runtime_abi.h`. The ABI
uses only C fixed-width scalar types, `size_t`, `intptr_t`, `uintptr_t`, and raw
pointers. It does not expose C++ types, exceptions, standard-library objects,
or name mangling.

`stringv` crosses the boundary as two arguments: a pointer to immutable bytes
and an exact byte length. A terminating null byte is neither required nor
included in the length.

The compiler emits one typed output call for each supported Flecha scalar
value. `stdout` and `stdoutln` accept zero or more printable values and emit
them in argument order; `stdoutln` then emits
`flecha_runtime_v1_stdout_newline` once. `stdoutf` parses its string-literal
format at compile time and lowers literal segments and substituted values to
the same typed calls. It supports `%d` for numeric values, `%s` for `string`,
`stringv`, or `char`, `%b` for `bool` or `u8`, and `%%` for a literal percent
sign. `stdoutf` does not append a newline. No variadic C ABI is exposed by
these language-level facilities.

Object-only compilation intentionally does not need the runtime archive and
may leave runtime symbols unresolved. Executable builds automatically link the
archive configured with the compiler. A missing archive or linker failure is
reported with a structured `FLECHA-C` diagnostic.

Breaking an existing declaration requires a new ABI version and symbol prefix.
New symbols may be added to version 1 without changing existing declarations.

The runtime also owns dynamic safety checks. Version 1 provides checked
`stringv` byte access and substring bounds validation. Invalid indices print a
runtime diagnostic to standard error and terminate the process rather than
allowing an out-of-bounds memory access.

## Owning string ABI

The owning `string` type is specified in [`string.md`](string.md). Its
ABI representation contains a mutable data pointer, byte length, and capacity.
Runtime entry points construct a string from a view, clone it, concatenate
values, and destroy its allocation. Constructors and formatters receive an
explicit output pointer rather than returning the three-field structure by
value. This avoids platform-specific aggregate return conventions and keeps
the compiler/runtime boundary stable.

No C++ standard-library type or exception may cross this boundary. Allocation
failure and invalid dynamic operations must use the runtime failure mechanism.
Object-only compilation may leave owning-string runtime symbols unresolved in
the same way it currently leaves output and bounds-check symbols unresolved.

## File reading

`flecha_runtime_v1_read_file` receives an owning-string output pointer plus
explicit path bytes and length. POSIX treats paths as native byte strings;
Windows validates UTF-8 and converts to UTF-16 before opening. Embedded nulls
and empty paths are invalid on every platform. Files are read in binary mode,
so content nulls and line endings are preserved.

Success transfers one malloc-compatible, null-terminated allocation into the
output descriptor. Every failure leaves the descriptor empty, closes the file,
and releases partial storage. Stable status codes distinguish not found,
permission denied, invalid path, generic read failure, the current `u32` string
length limit, and allocation failure. The safe `filesystem::read_file` wrapper
maps those codes to `IoError` or `AllocationError.OutOfMemory`.

## Structure values (compiler-private ABI)

Flecha structures use named, non-packed LLVM structures, preserving field order
and native alignment. References/raw pointers to structures use opaque native
pointers. This does not establish a portable C aggregate ABI or expose field
layout inspection to Flecha source.

A function whose result contains a structure (including an array or tuple of
structures) returns LLVM `void` and receives an additional first pointer argument
for its result storage. The caller provides final storage, and the callee
constructs into it. This preserves the address of immovable objects from
construction through tear, including nested factory calls. These results must
not be called from a C harness as ordinary C aggregate returns. Tests expose
scalar wrappers instead.

Structure parameters use LLVM aggregate values. For a copyable structure, the
callee clones owned fields on entry and cleans up that copy on exit. For a
noncopyable movable structure, the caller transfers the aggregate bits and
clears its source live state; the callee becomes the sole cleanup owner.
Structures with an unsafe tear remain immovable and cannot be parameters by
value. Compiler-generated calls invoke tear with an address, then recursively
destroy fields in reverse order. The same transfer rule applies recursively to
tuples and fixed arrays containing structures. Existing scalar, standalone
string, and structure-free tuple conventions are unchanged.

## Raw allocation storage ABI

`flecha_runtime_v1_alloc(size_t size, size_t alignment)` returns a pointer to
zeroed storage or null. Alignment must be a nonzero power of two. Size arithmetic
is overflow-checked. Zero-sized requests reserve at least one byte. The runtime
supports over-alignment by retaining the original malloc allocation in a hidden
header before the aligned result; this header is private to the runtime.

`flecha_runtime_v1_free(void* pointer)` releases that storage and accepts null.
Only exact live pointers returned by the matching allocator may be passed to it.
The runtime performs no Flecha teardown: generated code runs typed teardown
before calling free. `new<T>` supplies T's size and alignment from the native
LLVM DataLayout, branches on null, constructs only on success, and packages the
result into `(*T, error)`. Allocation errors use the existing generic error ABI.

The allocation-failure test harness wraps the allocation symbol at link time;
production code has no failure-injection setting or test-only global state.

Closure environments use the same allocator. Because creating a callable value
has no language-level error result, a null environment allocation calls
`flecha_runtime_v1_allocation_failure()`, which prints a runtime diagnostic and
terminates. Generated destroy thunks recursively tear down captured owning
fields in reverse capture order before calling `flecha_runtime_v1_free`.

Runtime scalar arrays use `flecha_runtime_v1_alloc_array(count, element_size,
alignment)`, which checks multiplication before allocating zeroed storage.
`flecha_runtime_v1_allocation_count(pointer)` returns the recorded count (zero
for null); it requires a Flecha allocation base. New generated code carries an
array owner's count in its `*[]T` value and does not call this function for owner
indexing or slicing. The query and hidden count remain in ABI v1 for legacy raw
`*T` indexing and source-compatibility conversions. Ordinary allocations record
a count of one. The hidden allocator header stores both the original base and
element count. Previously built objects and runtime libraries should be rebuilt
together. `free` retains the same public ABI and accepts either form.
