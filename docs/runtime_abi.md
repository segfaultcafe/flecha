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
value. `stdout`, `stdoutln`, `stderr`, and `stderrln` accept zero or more
printable values and emit them in argument order. The `ln` forms then emit the
corresponding `flecha_runtime_v1_*_newline` operation once. `stdoutf` and
`stderrf` parse their string-literal format at compile time and lower literal
segments and substituted values to typed stream calls. Each `{}` consumes one
printable value; `{{` and `}}` emit literal braces. The formatted operations do
not append a newline.

`__sprintf` uses the same compile-time parser and appends typed values to an
internal fallible format buffer. Finishing transfers one malloc-compatible
allocation to an owning Flecha string. Failure releases partial storage and
produces `AllocationError.OutOfMemory`. No variadic C ABI or runtime type-erased
argument pack is exposed by any of these language-level facilities.

`stdin` and `stdinln` lower their prompt values to typed stdout operations,
flush stdout, and call `flecha_runtime_v1_standard_input_read_line`. The line
reader consumes the same descriptor-level primitive exposed by
`flecha_runtime_v1_standard_input_read`, so switching between typed line input
and `std::io::stdio::StandardInput` cannot strand bytes in a separate native
read-ahead buffer. It removes LF and a preceding CR, distinguishes immediate
EOF from a final unterminated line, and transfers malloc-compatible
owning-string storage only on success. Typed
`flecha_runtime_v1_input_parse_*` operations consume the complete line. Their
stable status table is success `0`, EOF `1`, read failure `2`, invalid syntax
`3`, out of range `4`, prompt flush failure `5`, and out of memory `6`. Code
generation stores into the referenced destination only on success and maps
failures to `InputError` or `AllocationError`.

The standard I/O library uses additive byte-read, byte-write, and flush
operations for process-owned streams. A read or write resets its count output
before validation and performs one descriptor operation that preserves
partial progress on failure. Writes first flush typed built-in output already
pending in the selected C stream. Null storage is valid only with zero length;
a successful zero-byte read is EOF. These operations do not transfer ownership
of or close the process streams.

The compiler keeps a parameterless Flecha `main` independent from the host C
entry ABI. For a `void` or `i32` entry point it emits an `i32 main(i32, char**)`
wrapper that first passes the host argument table to
`flecha_runtime_v1_process_arguments_initialize` and then invokes the Flecha
function. The runtime borrows that table without modifying or releasing it;
the host retains ownership for the process lifetime. Argument index zero is
the invocation name supplied by the host. Count does not allocate, and capture
copies the selected null-terminated host argument into an independent owning
Flecha string. Capture status is success `0`, out of memory `1`, or invalid
index `2`; the caller supplies an empty string output, and every failure leaves
it empty. Argument bytes are preserved exactly as supplied by the host C entry
environment and are not normalized or validated as Unicode.

The environment ABI reads and mutates the current process environment through
explicit name and value byte ranges. `environment_get` clears its owning
string and presence outputs before validation. Success with presence zero
means that the variable is absent; success with presence one and an empty
string means that the variable exists with an empty value. A present value is
copied into independent owning storage. `environment_set` prepares complete
native name and value copies before mutation, and `environment_remove` treats
an absent variable as success. Allocation failure therefore leaves the
existing environment unchanged, while every failed lookup leaves both outputs
empty.

Names must be nonempty and cannot contain `=` or a null byte. Values may be
empty but cannot contain a null byte. POSIX hosts preserve name and value bytes
as supplied. Windows validates UTF-8 and uses the wide-character environment
API. Status values are success `0`, out of memory `1`, invalid name `2`,
invalid value `3`, permission denied `4`, and operation failure `5`.
Environment mutation is process-wide; callers must not concurrently read and
mutate the environment without external synchronization.

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

## Floating-point text conversion

The version 1 runtime exposes typed `f32` and `f64` parsing and formatting
entry points for the standard text library. Input is an explicit byte pointer
and length; parsing consumes the complete input, ignores no whitespace, and is
independent of the process locale. Decimal input accepts an optional sign,
fraction, and decimal exponent. The only non-finite spellings are `nan`, `inf`,
and `-inf`, with an optional leading plus accepted by parsing.

Formatting produces lowercase non-finite spellings and otherwise emits the
shortest decimal representation that round trips to the same width. Negative
zero retains its sign. The destination is unchanged when it is too small, and
the written-length output is zero on every failure. Stable status values
distinguish invalid syntax, an out-of-range value, insufficient space, and
failure to allocate the parser's null-terminated input copy. The safe `strings`
wrappers map them to `ParseError`, `FormatError`, and `AllocationError`.

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

`flecha_runtime_v1_path_style` reports which lexical grammar matches the
runtime's filesystem operations: `0` for POSIX and `1` for Windows. It performs
no allocation or filesystem access. Path joining, normalization, and component
inspection remain Flecha standard-library code; the runtime supplies only this
host-policy fact.

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

`flecha_runtime_v1_temporary_directory_create` atomically creates a new
directory in the host temporary directory and returns its absolute path as an
owned Flecha string. The path uses native bytes on POSIX and UTF-8 on Windows.
POSIX creation uses mode `0700`; Windows names use operating-system
cryptographic randomness and exclusive directory creation. Any failure removes
the created directory, frees partial output, and resets the string descriptor.
Recursive ownership and best-effort teardown remain in
`std::io::file::TemporaryDirectory`.

The `flecha_runtime_v1_directory_*` entry points expose owning directory
iterators behind opaque `void*` handles. `open` clears its output before path
validation. `next` returns one independently allocated entry name, a byte type,
and a byte presence flag. Types are `0` for other entries, `1` for regular
files, `2` for directories, and `3` for symbolic links or Windows reparse-point
links; classification does not follow the entry link. Successful end of
iteration has a zero presence flag and empty outputs. The caller must provide
an empty owning-string descriptor.

An entry-name allocation failure leaves the current entry pending so a later
`next` call can retry it without dropping or reordering entries. Other entry
errors consume that entry and leave the iterator open. `close` always consumes
the iterator handle, including when the operating system reports a close
failure. The runtime returns `.` and `..` when the host iterator provides them;
the safe Flecha wrapper filters those names.

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

`flecha_runtime_v1_temporary_file_open` atomically creates a new readable and
writable file in the host temporary directory and returns both a file-stream
handle and its absolute path. The path uses native bytes on POSIX and UTF-8 on
Windows. POSIX creation uses mode `0600`; all hosts use exclusive creation and
a non-inheritable descriptor. The caller owns both outputs on success. Any
failure closes the descriptor, removes the path, frees partial allocations, and
clears both outputs. Safe ownership, explicit removal, and deterministic
best-effort cleanup remain in `std::io::file::TemporaryFile`.

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
`lend` raw-resource fields and checked-reference fields are non-owning pointers
or borrowed descriptors and are not torn down; compile-time lifetime
validation ensures their targets remain live. `lend` applies only to the raw
resource form: checked references are inherently borrowed and never spell
`lend &T`.

Runtime scalar arrays use `flecha_runtime_v1_alloc_array(count, element_size,
alignment)`, which checks multiplication before allocating zeroed storage.
Zero-sized elements reserve one physical byte while preserving the requested
logical element count, so distinct slots may share an address without carrying
state.
`flecha_runtime_v1_allocation_count(pointer)` returns the recorded count (zero
for null); it requires a Flecha allocation base. New generated code carries an
array owner's count in its `*[]T` value and does not call this function for owner
indexing or slicing. The query and hidden count remain in ABI v1 for legacy raw
`*T` indexing and source-compatibility conversions. Ordinary allocations record
a count of one. The hidden allocator header stores both the original base and
element count. Previously built objects and runtime libraries should be rebuilt
together. `free` retains the same public ABI and accepts either form.
