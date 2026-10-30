# Operating-system context

Import `lib::std::os::arguments` to capture the command-line arguments
supplied to the current program:

```flecha
@imports :: lib::std::os::arguments;

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

## Environment variables

Import `lib::std::os::environment` to inspect or mutate the current process
environment. `get` returns `(string, bool, error)`: the string is an independent
owning copy, and the boolean distinguishes an absent variable from a present
variable containing an empty value. Failed reads return an empty string and
false.

```flecha
@imports :: lib::std::os::environment;

home, present, err := environment::get("HOME");
if err == none and present {
    stdoutln(home);
}
```

`set` copies its borrowed name and value before changing the process-wide
environment. An empty value is valid. `remove` is idempotent when the variable
is already absent. Names must be nonempty and cannot contain `=` or a null
byte; values cannot contain a null byte. Validation or allocation failure
leaves the previous environment unchanged.

POSIX preserves environment bytes as supplied. Windows accepts UTF-8 and uses
the operating system's wide-character environment API. Environment mutations
are immediately visible to later calls and to subsequently created child
processes. Concurrent reads and mutations require external synchronization.

`EnvironmentError` distinguishes invalid names, invalid values, permission
denial, and unclassified read or write failures. Allocation failure is reported
as `AllocationError.OutOfMemory`.

Child-process creation, exit-status handling, and pipes remain separate Stage
5 work under `std::process`.
