# Crashable functions

Crashability is an explicit function effect. A named function or method that
can reach an unrecoverable state places `!` after its name, and every call to
that function repeats the marker:

```flecha
void fail!() {
    crash("database invariant failed");
}

void start!() {
    fail!();
}
```

A normal function cannot call a crashable function. Calling a crashable
function without `!`, or calling a normal function with `!`, is a compile-time
error. This makes every propagation point visible in source rather than hiding
an unrecoverable effect inside an ordinary call.

The bang is part of function identity, not merely a call-site annotation.
`load()` and `load!()` may coexist with the same parameter types; the spelling
at the call selects the normal or crashable declaration before argument
checking. The same rule applies to methods, generic functions, module imports,
and native symbol generation. Paired forms should normally perform the same
operation with recoverable and unrecoverable failure policies rather than
represent unrelated behavior.

`crash(stringv message)` is the one spelling exception. It is already an
explicit terminal operation, so it is called without a bang, but only inside a
crashable function. `crash!()` is invalid. The operation never returns, so it
satisfies control-flow requirements for a non-void function path.

Callable types carry the same effect after their name:

```flecha
func FailureHandler!() => ();
```

Invoking a value of that callable type requires `handler!()` and a crashable
enclosing function. Crashability participates in callable type identity, so a
crashable function cannot be erased into a normal callable type.

## Crash paths and warnings

A crashable function should have a direct or transitive path to `crash()` or a
crashing compiler intrinsic. The compiler follows the call graph and warns:

```text
method 'name' is marked as crashable, while no path leads to a crashable state
```

This includes a recursive cycle made only of crashable calls with no terminal
crash. The diagnostic is a warning because removing the effect marker remains
a source-level API decision.

## Crash report

An explicit crash writes a report to standard error containing:

- the exact message bytes;
- the Flecha source file, one-based line and column, and enclosing function;
- a native stack trace;
- a register snapshot where the host exposes register context.

Linux provides register dumps for x86-64 and AArch64. Other targets retain the
source report and state when registers or native stack walking are unavailable.
The register snapshot is captured as the runtime reporter is entered. The
process then terminates with `SIGABRT`; Flecha does not manufacture `SIGSEGV`,
and Ctrl-C/`SIGINT` remains a normal external interruption.
