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

## Filesystem paths

The `flecha_runtime_v1_path_exists`, `metadata`, `remove_file`, `rename`, and
`create_directory` entry points use the same explicit path representation and
platform conversion as `read_file`. Existence checks report a missing path as
successful false. Metadata follows symbolic links and reports type `0` for
other entries, `1` for regular files, and `2` for directories. Its size output
is the regular-file byte length and zero for other types. Both metadata outputs
are cleared before path validation. Mutations report missing paths as
`NotFound`; exclusive directory creation reports an existing path as
`AlreadyExists`.

The filesystem status table matches the file-stream table: success `0`, not
found `1`, permission denied `2`, invalid path `3`, already exists `4`, too
large `5`, out of memory `6`, invalid options `7`, and operation failure `8`.
`remove_file` does not remove directories. Directory creation is non-recursive
and uses mode `0777` through the process umask on POSIX.

`flecha_runtime_v1_remove_directory` accepts independent byte booleans for
force and recursive behavior. Non-recursive removal requires an empty
directory. Recursive removal never treats a top-level file as a directory.
Force suppresses `NotFound`; recursive traversal otherwise preserves the same
status mapping. `DirectoryNotEmpty` is status `9`, leaving generic operation
failure at its established status `8`.

## File streams

The `flecha_runtime_v1_file_stream_*` entry points expose unbuffered binary file
handles behind opaque `void*` values. Paths use the same explicit bytes and
platform rules as `flecha_runtime_v1_read_file`. `open` clears its output before
validation and transfers one runtime-owned handle only on success. `close`
always consumes that handle, including when the operating system reports a
close failure.

Open flags are byte booleans and accept only zero or one. At least one of read
or write is required; append, truncate, create, and create-new require write
access. Create-new uses the operating system's exclusive-create operation, so
checking and creation are atomic. POSIX file creation applies mode `0666`
through the process umask. Windows paths are validated as UTF-8 and opened as
UTF-16.

Read and write each perform one system operation, retrying interruptions before
any transfer. Their count output is reset before validation and reports a
successful partial transfer normally. A null data pointer is accepted only for
a zero-length operation. Seek origins are `0` for start, `1` for current, and
`2` for end. Flush synchronizes writable handles with the backing file and is a
no-op for read-only handles.

File-stream status values are stable: success `0`, not found `1`, permission
denied `2`, invalid path `3`, already exists `4`, too large `5`, out of memory
`6`, invalid options `7`, and operation failure `8`. This table is distinct
from the older read-file-specific status enum, whose value `4` means a generic
read failure.

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

## Callable values (compiler-private ABI)

Every callable value has the uniform compiler-private layout `{invoke,
environment, destroy}`. `invoke` receives the environment pointer before the
declared Flecha parameters. Named functions and captureless anonymous
functions store null environment and destroy pointers. Capturing anonymous
functions store a heap environment plus a typed destroy thunk.

Callable values are move-only, including transitively through structures,
tuples, and fixed arrays. A transfer clears the complete source value so only
the destination can invoke its destroy thunk. Calls, returns, generic
specializations, module boundaries, aggregate fields, and methods all retain
the same three-pointer representation. This is not a C callback ABI or a
stable cross-version binary interface.

Reusable and `once func` values use this same layout; single-use is a semantic
type property and does not add a callable field. Invoking a once value transfers
the three pointers, calls `invoke`, then calls `destroy` before continuing.
Owned structure captures add compiler-private liveness bits inside the closure
environment so the destroy thunk skips values moved out by the once body while
still destroying untouched or never-invoked captures.

Release-package validation executes captureless and capturing callables across
modules, methods, generic transfers, tuples, arrays, and owned-resource
captures on every supported native target. Leak-sanitized native tests verify
that moved environments and captured resources are released exactly once.

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
terminates. Copy-captured owning fields and generalized `own` values are
recursively torn down in reverse capture order before
`flecha_runtime_v1_free`. This includes nested callable environments,
structures, tuples, and fixed arrays. An `own` raw-resource field is released
in the same order, using its pointee's typed teardown before the runtime free.
`lend` raw-resource and checked-reference fields are non-owning pointers or
borrowed descriptors and are not torn down; compile-time lifetime validation
ensures their targets remain live.

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
