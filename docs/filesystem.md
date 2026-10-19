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

The `errors` module exports the `IoError` family containing `NotFound`, `PermissionDenied`,
`InvalidPath`, `ReadFailed`, and `TooLarge`. Allocation failure uses the shared
`AllocationError.OutOfMemory` value. On every failure the returned string is
empty. `InvalidPath` includes empty paths, embedded nulls, and invalid UTF-8 on
Windows. `TooLarge` means the contents cannot fit the current byte-string
length plus its required terminator.

POSIX paths are passed to the operating system as byte sequences. Windows paths
must be UTF-8 and are converted to UTF-16 before opening. The function closes
the file and releases any partial buffer on every exit. No Unicode decoding or
newline conversion is performed on file contents.

The implementation is an ordinary safe Flecha wrapper around the versioned
`flecha_runtime_v1_read_file` C ABI. The wrapper initializes an empty owning
string before passing its address, and the runtime either transfers exactly one
compatible allocation into that slot or leaves it empty. Generated string
cleanup therefore owns and destroys the result exactly once.

## File streams

Import `lib::std::io::file` for incremental binary I/O. `File::open` accepts an
`OpenOptions` value; the `readOnly`, `writeOnly`, and `appendOnly` constructors
cover the common modes. `writeOnly` creates and truncates, while `appendOnly`
creates and preserves existing contents. Custom options can request combined
read/write access, non-truncating creation, or atomic `createNew` behavior.

`File` owns its opaque native handle and closes it during teardown. Explicit
`close` consumes the handle even when closing reports an error. `read` and
`write` accept checked byte slices, return the transferred byte count, and
preserve partial counts alongside errors. A successful read of zero bytes means
end of file. `seek`, `flush`, `close`, and operations on a closed handle report
through the `IoError` family.
