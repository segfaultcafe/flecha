# Flecha standard-library development guidelines

These guidelines apply to Flecha source in `std/`. Follow the naming conventions
in [library API style](library_api_style.md) and the architectural conventions
in the [Idiomatic Flecha Guideline series](IFG/IFG_2026_0000.txt) as well.

## Set up a standard-library checkout

The focused targets configure and build `build-tests` when needed, so a clean
checkout can start directly with a test command. The host still needs the
compiler, LLVM, CMake, and build-tool requirements listed in the main
[README](../README.md#requirements).

Run the Flecha-authored standard-library module tests and verify that the
checked-in API reference matches the sources:

```sh
make test-stdlib
```

This is the normal pre-pull-request check for a change contained within
`std/src`. It compiles every logical test module once through Longbow. It does
not replace native/runtime fixtures, sanitizers, cross-platform CI, or the full
repository gate when those checks apply.

## Iterate on one module

During implementation, run the narrowest relevant test source:

```sh
make test-stdlib-one \
    STDLIB_TEST=std/src/encoding/base64_test.fl
```

The selected file runs through `flecha test` with `std/src` as both the module
root and `std` library. File-local selection executes the tests declared in
that source; it does not automatically run every test in the same logical
module. Use `make test-stdlib` before opening a pull request to cover attached
sources and the complete Flecha-authored standard library.

After editing, use the repository change detector for the affected compiler,
runtime, documentation, and packaging checks:

```sh
make test-plan
make test-changed
```

`make test-plan` explains the selection without building. Because standard
library imports cross module boundaries, a change under `std/` conservatively
selects the full repository suite; the focused targets shorten iteration but
do not weaken that integration gate.

## Run native and runtime fixtures

Standard-library modules that cross the runtime ABI or depend on native
allocation, files, directories, processes, clocks, randomness, networking, or
other host behavior have CTest-managed fixtures. Run the complete focused set
with:

```sh
make test-stdlib-runtime
```

The target selects `StandardLibraryTests.*` through CTest and excludes the
Flecha-module aggregate already covered by `make test-stdlib`. To investigate
one fixture, list and select its exact CTest name:

```sh
ctest --test-dir build-tests -N -R '^StandardLibraryTests\.'
ctest --test-dir build-tests --output-on-failure \
    -R '^StandardLibraryTests\.FilesystemReadFile$'
```

Add or extend a native/runtime fixture when behavior cannot be proven by a
Flecha test alone. In particular, owning-result and fallible-allocation changes
need injected allocation-failure coverage that verifies empty or valid partial
results, initialized-prefix invariants, and exactly-once cleanup.

## Document public contracts at their declarations

Place `//` contract comments immediately above every exported function, type,
callable alias, error family, and type set, as well as every public method on an
exported type. Document public fields and error or enum variants when their
meaning is not completely captured by the declaration.

Comments should state observable behavior: ownership or borrowing, mutation,
partial-result rules, ordering, error conditions, and whether failure preserves
the receiver or destination. Do not merely expand the declaration's name, and
do not promise implementation details that callers cannot rely on. Private
helpers need comments only when their invariants or purpose are otherwise
unclear.

## Update the public API reference

Every file that adds, removes, or changes an `export` declaration or a public
method on an exported type also updates the matching entry in
[`standard_library_modules.json`](standard_library_modules.json). Regenerate
the checked-in declaration index with:

```sh
python3 tools/generate_std_api_reference.py
python3 tools/generate_std_api_reference.py --check
```

The generator requires an exact match between public modules discovered under
`std/src` and the metadata manifest. Pull-request CI runs the check, while the
installed `examples/standard_library` fixture verifies the documented import
layout against every release bundle.

`make test-stdlib` runs the `--check` form automatically. Run the generator
without `--check` after changing public declarations or module metadata, review
the resulting documentation diff, and then rerun the focused target.

## Choose the final validation level

Use the narrowest checks while iterating, then expand validation according to
the boundary changed:

- Run `make test-stdlib` for every standard-library change.
- Run `make test-stdlib-runtime` for runtime-ABI, host-I/O, allocation, or
  platform-backed modules.
- Add and run allocation-failure fixtures for new owning results, growth paths,
  and other fallible allocation.
- Run `make test-sanitizers` for memory, ownership, cleanup, unsafe, native
  runtime, or FFI changes.
- Run `make test` for broad integration work when reusable cached successes are
  appropriate. Use `make test-full` for release qualification or another
  explicit uncached full-suite requirement; CI always applies that policy.
- Rely on the Linux, Windows, and macOS CI matrix for changes to filesystem,
  path, process, network, time, runtime ABI, toolchain, or packaging behavior.
  Do not claim portable support from one host run.

Before opening a pull request, update the module metadata and API reference,
run the applicable focused commands above, run `make test-changed`, and record
the commands and results in the pull-request description. The complete CI and
installed-package qualification remain the merge gate.

## Expand nonempty brace-delimited bodies

Never put a nonempty body on one line, even when it contains only one statement.
Put the opening brace on the declaration or control-flow line and the closing
brace on its own line. An empty body may use `{}` on the same line.

```flecha
i32 test() {
    return 1;
}

void empty() {}
```

This applies to functions, structures, conditionals, loops, unsafe blocks,
`build` expressions, error declarations, and every other brace-delimited body.
For example, `return build RawArray<T> {};` is allowed because its body is empty.

Structure declarations retain their required trailing semicolon:

```flecha
structure Counter {
    i32 value;
};
```

## Separate top-level declarations

Leave at least one blank line and at most two blank lines between top-level
declarations. Keep consecutive structure fields together without blank lines.
Do not place multiple declarations or statements on the same line.

```flecha
structure Position {
    i32 x;
    i32 y;
};

i32 first() {
    return 1;
}

i32 second() {
    return 2;
}
```

## Group statements by purpose

Keep statements that perform one related task together. Use a blank line when
the code moves to a different task. Do not compress unrelated operations into a
single uninterrupted block or separate every statement mechanically.

Keep an operation and its immediate error check together. Keep creation and
configuration of the same value together. Separate a completed conditional from
the next distinct operation.

The following layout illustrates those groups; the request functions and types
are placeholders:

```flecha
if ready {
    prepare();
}

y := x * factor;

req := request();
req.status = 300;

resp, err := response();
if err != none {
    return err;
}
```
