# Filesystem

Import the standard I/O modules with `lib::std::io::filesystem` and
`lib::std::io::errors`:

```flecha
@imports
    :: lib::std::io::filesystem
    :: lib::std::io::errors;

contents, err := filesystem::read_file("input.fl");
if err != none {
    return err;
}
```

`export (string, error) read_file(stringv path)` reads the entire file in binary
mode. The returned string preserves every byte, including embedded nulls and
platform line endings. An empty file succeeds with an empty owned string. Safe
callers do not need an unsafe block.

The `errors` module exports the `IoError` family containing `NotFound`,
`PermissionDenied`, `InvalidPath`, `ReadFailed`, `UnexpectedEof`, and
`TooLarge`. Allocation failure uses the shared `AllocationError.OutOfMemory`
value. On every `read_file` failure the returned string is empty. `InvalidPath`
includes empty paths, embedded nulls, and invalid UTF-8 on Windows. `TooLarge`
means the contents cannot fit the current byte-string length plus its required
terminator.

POSIX paths are passed to the operating system as byte sequences. Windows paths
must be UTF-8 and are converted to UTF-16 before opening. The function closes
the file and releases any partial buffer on every exit. No Unicode decoding or
newline conversion is performed on file contents.

The implementation is an ordinary safe Flecha wrapper around the versioned
`flecha_runtime_v1_read_file` C ABI. The wrapper initializes an empty owning
string before passing its address, and the runtime either transfers exactly one
compatible allocation into that slot or leaves it empty. Generated string
cleanup therefore owns and destroys the result exactly once.

## Lexical paths

Import `lib::std::io::path` for host-aware joining, normalization, filename,
parent, and extension operations. These are lexical operations: they do not
access the filesystem, resolve symbolic links, or make paths absolute. The
allocating operations report allocation failure, while component queries
return borrowed views into their source. See [Lexical paths](path.md) for the
complete POSIX and Windows contracts.

`export error write_file(stringv path, stringv contents)` creates or truncates
a file, writes the complete string view, flushes it, and closes it. String
literals can be written directly, and embedded null bytes are preserved.
`write_bytes` provides the same operation for `[]const u8` buffers. Opening,
writing, flushing, and closing failures are returned to the caller. Empty
inputs create an empty file, and data is written without newline or encoding
conversion.

`exists` returns `(false, none)` when a path is absent and reserves errors for
invalid paths or operating-system failures. `remove_file` removes files but not
directories. `rename` accepts files or directories and follows the platform's
replacement rules when the destination already exists. `create_directory`
creates one directory level with permissions filtered through the process
umask on POSIX. It reports `AlreadyExists` for an existing path and `NotFound`
when a parent component is missing.

`metadata` returns a `Metadata` value containing `file::EntryType.Other`,
`file::EntryType.File`, or `file::EntryType.Directory`. Symbolic links are
followed. Regular file sizes are reported in bytes; directories and other entry
types have size zero. A missing path reports `NotFound`.

`create_directories` creates every missing component of a path. It succeeds
when the final path is already a directory and reports `AlreadyExists` when an
existing component is not a directory. The path is lexically normalized before
the first mutation, so normalization allocation failure creates nothing.
Failure during creation may leave successfully created parent directories in
place. As with `metadata`, an existing symbolic link to a directory is followed.

`copy_file(source, destination)` copies the bytes of one regular file to a new
destination. It never replaces an existing entry; that case reports
`AlreadyExists`. The source follows ordinary open semantics, including a
symbolic link to a regular file. One 32 KiB byte buffer is allocated in Flecha
before destination creation, then `stream::copy` handles partial reads and
writes without dropping, duplicating, or reordering bytes. The destination is
explicitly flushed and both files are closed. An allocation failure creates no
destination, while a later read, write, flush, or close failure may leave a
partial destination file.

`copy_directory(source, destination)` recursively copies a directory into a
new destination whose parent must already exist. The root source follows a
symbolic link to a directory, but symbolic links and unsupported entry kinds
encountered inside the tree report `UnsupportedFileType` rather than being
followed or recreated. Empty directories and empty files are preserved.
Existing destinations report `AlreadyExists`. File permissions, timestamps,
link identity, and other metadata are not copied.

Directory copying allocates one shared 32 KiB Flecha buffer before creating the
destination. Entry-name and joined-path allocations can still fail later, and
any error may leave a partially populated destination for the caller to inspect
or remove. Before creation, a narrow runtime query resolves existing path
components and rejects a destination equal to or below the source, including a
path reached through a symbolic-link alias. Like ordinary filesystem APIs, this
check is not a synchronization primitive: callers must not concurrently replace
path components or move the source while copying.

`remove_directory(path, false)` removes an empty directory and reports
`DirectoryNotEmpty` when it still contains entries. Passing `true` recursively
removes the directory tree and treats an already-missing path as success. A
non-forced removal of a missing directory reports `NotFound`.

## Directory iteration

Import `lib::std::io::file` and call `file::Directory::open(path)` to create an
owning directory iterator. `Directory::next` returns
`(DirectoryEntry, bool, error)`. A true boolean accompanies one entry; a false
boolean with no error is end of iteration. End of iteration is stable, so later
calls continue returning no entry. The wrapper omits the host-provided `.` and
`..` entries.

Each `DirectoryEntry` owns its name independently of the iterator. `name()`
borrows that name without allocating, while `kind()` returns an `EntryType`:
`Other`, `File`, `Directory`, or `SymbolicLink`. Classification does not follow
symbolic links; Windows reparse-point links are reported as `SymbolicLink`.
The owning `entryName` and `entryKind` fields contain the same values for code
that needs direct value access. Entries are move-only so returning or storing an
entry transfers its owned name without an unreported allocation.

`Directory` is move-only and closes its opaque host iterator during
deterministic teardown. Explicit `close` consumes the handle even when the
operating system reports a close failure. `next` and repeated `close` calls on
a closed value report `IoError.Closed`. An allocation failure while creating an
entry name leaves that entry pending, so retrying `next` cannot drop or reorder
it. Other iteration errors leave the directory open. Opening, iteration, and
closing may block.

## File streams

Import `lib::std::io::file` for incremental binary I/O. `File::open` accepts an
`OpenOptions` value; the `readOnly`, `writeOnly`, and `appendOnly` constructors
cover the common modes. `writeOnly` creates and truncates, while `appendOnly`
creates and preserves existing contents. Custom options can request combined
read/write access, non-truncating creation, or atomic `createNew` behavior.

`File` owns its opaque native handle and closes it during teardown. Explicit
`close` consumes the handle even when closing reports an error. `read` and
`write` accept checked byte slices, return the transferred byte count, and
preserve partial counts alongside errors. `writeAll` repeatedly writes the
unwritten suffix until the complete slice has been transferred. It reports
`WriteFailed` if a successful write makes no progress, preventing an infinite
loop. `readExact` similarly fills its complete destination and reports
`UnexpectedEof` if the file ends first. A successful ordinary read of zero
bytes means end of file. `seek`, `flush`, `close`, and operations on a closed
handle report through the `IoError` family.

`File::reader` and `File::writer` adapt a file to `io::stream::Reader` and
`io::stream::Writer`. The adapters forward partial byte counts and errors to
the stream operations, allowing files to participate in generic operations
such as `stream::copy`. Each adapter mutably borrows its file and remains
move-only with the underlying stream callable. The file cannot be accessed,
moved, or closed while that borrow remains live. An adapter neither owns nor
closes the file, and a writer does not flush automatically; callers retain
responsibility for flushing and closing after the adapter's final use.

### Temporary files

Temporary files are part of `lib::std::io::file`; there is no separate `temp`
module. `TemporaryFile::create()` atomically creates a new readable and writable
file in the host temporary directory. On success the owner contains an ordinary
`File` plus the generated absolute path. Creation can report the same stable
open and allocation errors as `File::open`, and every failed creation closes
and removes any partially created file.

`file()` returns a mutable borrow of the underlying `File`, so reads, writes,
seeks, stream adapters, flushing, and explicit close follow the normal file
contracts. The borrow prevents moving, removing, or otherwise mutating the
`TemporaryFile` until its final use. `path()` returns a borrowed string view and
does not allocate.

`remove()` closes an open handle, then removes the path. It is idempotent, and
an already-missing path counts as removed. If removal fails, the path remains
owned so a later `remove()` or deterministic teardown can retry it. A close
failure consumes the handle as with `File::close`; removal is still attempted,
and the close error takes precedence when both operations fail. Destruction
cannot report errors, so it closes and removes best-effort. Moving a
`TemporaryFile` transfers the handle, path, and cleanup responsibility exactly
once.

### Temporary directories

Temporary directories also live in `lib::std::io::file`.
`TemporaryDirectory::create()` atomically creates a private directory in the
host temporary directory and returns its generated absolute path as an owned
resource. POSIX creation uses owner-only permissions; Windows creation uses an
unpredictable name and the temporary directory's inherited access rules.
Creation can report stable filesystem or allocation errors, and every failed
creation removes any directory created before the failure.

`path()` returns a borrowed view without allocating. The borrow prevents
moving or removing the owner until its final use. `remove()` recursively
removes the complete directory tree, treats an already-missing path as
success, and clears the owned path only after successful removal. A failure
keeps the path armed so the caller can repair the cause and retry.

Deterministic teardown performs the same recursive removal best-effort because
destruction cannot report errors. Moving a `TemporaryDirectory` transfers its
path and cleanup responsibility exactly once. A default-constructed value has
an empty path, owns nothing, and can be removed repeatedly as a no-op. Call
`remove()` explicitly when cleanup failure must be observed.

## Standard output and error

Import `lib::std::fmt` for ordinary typed program output. Its `print`,
`println`, and `printf` functions write to standard output; `eprint`,
`eprintln`, and `eprintf` provide the corresponding standard-error operations.
See [Formatting and console output](formatting.md) for the format syntax and
allocation behavior. These conveniences do not expose partial progress or
recoverable output errors.

Import `lib::std::io::stdio` when process standard streams must compose with
fallible byte-oriented I/O:

```flecha
@imports
    :: lib::std::io::buffered
    :: lib::std::io::stdio;

output := stdio::standard_output();
writer, allocationError := buffered::BufferedWriter::withCapacity(
    output.writer(),
    4096
);
if allocationError != none {
    return allocationError;
}

// Write bytes through writer.
return writer.flush();
```

`standard_input()`, `standard_output()`, and `standard_error()` return
lightweight, non-owning views of process-owned streams. The views never close
a stream and require no destructor. Multiple views may coexist, but all input
views consume the same process input cursor. `StandardInput::read` accepts a
mutable byte slice, permits short reads, and reports EOF as `(0, none)`.
Its `reader()` method returns a captureless `io::stream::Reader` that composes
with `BufferedReader`. A zero-length read succeeds immediately without
blocking. Once a reader is buffered, continue consuming input through that
`BufferedReader`: its Flecha-owned read-ahead bytes are intentionally not
visible to another standard-input view or to the typed `stdin` built-ins.

The output views' `write` and `writeAll` methods accept strings; `writeBytes`
and `writeAllBytes` accept byte slices. All four preserve the same
partial-progress and zero-progress rules as `io::stream::Writer`. A
zero-length write is a successful no-op.

Each `writer()` call returns an independent captureless
`io::stream::Writer`; it does not borrow the view. The native write operation
first flushes typed built-in output pending in the same C stream, then performs
one unbuffered descriptor write. Consequently a buffered standard stream
requires only `BufferedWriter::flush()` to emit its pending bytes. Calling the
view's `flush()` explicitly is useful for output previously emitted through
the built-ins and reports `IoError.FlushFailed` when the process stream cannot
be flushed.

Standard-stream reads, writes, and flushes are blocking operations except for
zero-length operations. They do not validate text encoding or add line
endings. Use the `stdin` built-ins for complete-line typed conversion and
`std::fmt` when typed formatting or an appended newline is desired.

## Buffered streams

Import `lib::std::io::buffered` to wrap an existing `io::stream::Reader` or
`io::stream::Writer`. `BufferedReader::withCapacity` and
`BufferedWriter::withCapacity` require a nonzero capacity, allocate an owning
byte slice, and move the supplied stream adapter into the result. Zero capacity
reports `IoError.InvalidOptions`; allocation failure reports
`AllocationError.OutOfMemory`. The moved adapter is consumed and destroyed on
every construction outcome, so a failed construction releases any borrow it
carried exactly once.

A buffered wrapper owns its allocation and stream adapter but not the resource
borrowed by that adapter. For example, wrapping `File::reader()` moves the
file's mutable borrow through the stream reader into the buffered reader. The
file cannot be accessed, moved, closed, or adapted again until the buffered
reader's final possible use. Moving the buffered wrapper transfers both the
allocation and borrow without copying either one.

`BufferedReader::read` returns immediately from existing read-ahead data. When
the internal buffer is empty, destinations at least as large as the configured
capacity are read directly; smaller destinations cause one capacity-sized
read-ahead operation. Successful zero progress records end of input. Bytes
returned together with an underlying error remain readable, and that error is
deferred until the call that consumes the final buffered byte. `readExact`
repeats this operation and reports `IoError.UnexpectedEof` when input ends
before the destination is full. Zero-length reads succeed without touching the
underlying reader.

`BufferedWriter::write` buffers small inputs. When pending data prevents a new
input from fitting, it is drained before any byte from the new input is
accepted. Inputs at least as large as the capacity are written directly when
the buffer is empty, preserving the underlying partial count and error.
`writeAll` repeats partial operations until every source byte is accepted.

`BufferedWriter::flush` drains only the Flecha-owned byte buffer; the generic
`io::stream::Writer` abstraction has no operation for flushing an underlying
resource. A partial failure removes only the acknowledged prefix and preserves
the unwritten suffix in order, allowing a later `flush` to retry without
duplication. Successful zero progress reports `IoError.WriteFailed` and keeps
all unwritten data. An empty flush succeeds without calling the writer.

Destruction releases the owned allocation and stream adapter but deliberately
does not flush. Buffered data remaining at destruction is discarded because a
destructor cannot report an I/O error. Callers must explicitly invoke the
buffered writer's `flush`; after its final use releases a file borrow, they may
separately call `File::flush` and `File::close`.
