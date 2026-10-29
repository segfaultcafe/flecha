# Formatting and console output

Import `lib::std::fmt` for the standard formatting facade:

```flecha
@imports
    :: lib::std::fmt;

fmt::print("items: ", 3);
fmt::println(" ready");
fmt::printf("{} + {} = {}", 20, 22, 42);
```

`print` and `println` accept zero or more printable values and write them in
argument order without inserting spaces. `println` appends exactly one newline.
`eprint` and `eprintln` have the same behavior on standard error.

`printf` and `eprintf` require a string-literal format. Each `{}` consumes one
printable value. `{{` emits a literal left brace and `}}` emits a literal right
brace. An unmatched brace, a non-literal format, the wrong number of values, or
a non-printable value is rejected at compile time. The formatted functions do
not append a newline.

`sprintf` uses the same format syntax and returns `(string, error)`. It creates
one owning string containing the complete result. Allocation failure returns an
empty string with `AllocationError.OutOfMemory`; partial text is never exposed.

The `fmt` functions are compile-time forwarding wrappers over compiler
built-ins. Flecha emits typed operations for the concrete call arguments; no C
variadic call or runtime type-erased argument pack is part of the ABI.

Use `std::io::stdio` instead when a program needs fallible byte-oriented
standard streams, partial-progress reporting, or readers and writers that
compose with buffered I/O.

## Typed line input

The `stdin` and `stdinln` compiler built-ins read one complete line into an
already-declared mutable destination:

```flecha
i32 count;
error inputError = stdin("count: ", &count);

string name;
inputError = stdinln("name", &name);
```

Every argument before the final checked reference is a printable prompt value.
`stdin` writes the prompt as-is; `stdinln` appends one newline. Both flush
standard output before blocking. The destination's resolved type selects the
conversion: all integer and floating types, `bool`, `char`, and owning `string`
are supported. `x` must already have a type at the callsite; input never
declares or infers a variable.

Input removes the terminating LF and an immediately preceding CR. Conversion
consumes the complete remaining line and does not trim whitespace. Boolean
input is exactly `true` or `false`; character input must contain exactly one
byte. Owning strings preserve every other byte. EOF after bytes completes a
final unterminated line, while EOF before any byte reports
`InputError.EndOfFile`.

The built-ins return `error`. `InputError` also provides `ReadFailed`,
`InvalidSyntax`, `OutOfRange`, and `PromptFailed`; allocation failure is
`AllocationError.OutOfMemory`. The destination remains unchanged on every
failure. There is no formatted-input builtin: compose a formatted prompt
explicitly with `fmt::printf(...)` and then call `stdin(&value)`.

For raw bytes rather than typed complete-line conversion, use
`std::io::stdio::standard_input()`. Its non-owning `StandardInput` view exposes
`read` and a reusable `io::stream::Reader`; EOF follows the stream convention
of `(0, none)`.

An explicit typed declaration without `=` uses `none` as its initializer when
the resolved type is trivially zeroable, so `i32 count;` is exactly the
shorthand for `i32 count = none;`. Types whose default construction performs
work must spell `= none` explicitly. Inferred declarations still require an
initializer.
