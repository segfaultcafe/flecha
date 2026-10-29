# Process arguments

Import `lib::std::process::arguments` to capture the command-line arguments
supplied to the current program:

```flecha
@imports :: lib::std::process::arguments;

i32 main() {
    values, captureError := arguments::capture();
    if captureError != none {
        return 1;
    }

    loop usize index = 0; index < values.len(); index++ {
        stdoutln(values[index]);
    }
    return 0;
}
```

`export (own []string, error) capture()` returns the complete host argument
table. Index zero is the invocation name supplied by the host; subsequent
elements are the arguments passed by the shell, Longbow, or another process.
The invocation name is host-defined and is not guaranteed to be an absolute or
canonical path.

The returned array owns its storage, and every element is an independent
owning string. Moving the array transfers the complete allocation and all of
its strings. Letting it leave scope destroys every element and releases the
array storage exactly once. Calling `capture()` again creates another
independent snapshot: replacing a string in one result cannot change another
result or the host argument table.

Allocation failure returns an empty array with
`AllocationError.OutOfMemory`. Construction allocates the array first and then
captures each argument directly into its zero-initialized destination slot. If
capturing a later argument fails, previously captured strings and the array
allocation are released before the error is returned. Empty arguments are
preserved as empty strings.

Argument bytes are copied exactly from the process entry environment. Flecha
does not normalize paths, decode Unicode, or apply shell parsing. The host C
entry convention terminates each argument with a null byte, so an individual
argument cannot contain an embedded null. `capture()` does not read from a
stream and does not block on operating-system I/O.

Flecha keeps `main` parameterless. The compiler-generated native entry wrapper
records the host argument table before calling the Flecha entry point, and the
runtime borrows that table for the process lifetime. Application code accesses
only the safe owning snapshot returned by `arguments::capture`; the raw runtime
operations remain internal to `std`.

This module exposes command-line arguments only. Environment variables,
process creation, exit-status handling, and pipes are separate Stage 5 work.
