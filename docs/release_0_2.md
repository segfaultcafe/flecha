# Flecha 0.2.0 milestone

Flecha 0.2.0 is the callable-values and closures milestone. This document
separates implemented foundations from the remaining staged work.

## Callable syntax and contracts

Named callable signatures use:

```flecha
func Mapper(i32) => (i32);
func Logger(stringv) => ();
unsafe func Reader(lend *const u8 source) => (i32);
```

Captureless anonymous functions use the same signature shape:

```flecha
[](i32 value) => (i32) { return value * 2; }
(i32 value) => (i32) { return value * 2; }
Mapper inferred = (value) => { return value * 2; };
```

The empty capture list can be written as `[]` or omitted. An explicit literal
spells its complete signature. A contextually inferred literal uses bare
parameter names, omits the result list, and receives its complete signature
from a typed initializer, argument, or return context. Partial signature
inference is invalid. Named aliases are structural, while all callable values
are move-only.

Nonempty capture lists now support snapshot captures:

```flecha
Mapper scaled = [copy factor](value) => { return value * factor; };
```

`copy` is contextual inside capture lists, so existing variables named `copy`
remain valid elsewhere. Captures are explicit: an outer local omitted from the
list is not visible in the body. Copyable scalars and aggregates are captured
by value, owning fields are cloned, and the snapshot remains valid after the
creating scope exits. Borrowed handles, callable values, and other noncopyable
values are rejected by this mode. A raw pointer may be copied only by an
`unsafe` anonymous function.

Checked-reference captures use `&name` for an exclusive mutable loan and
`&const name` for a shared read-only loan. Their origins follow closure moves,
callable returns, and callable-containing arrays, tuples, and structures. The
borrow checker rejects storage and return escapes, keeps loans active until the
closure's last use, and uses `<- parameter` contracts to describe borrowing
callable results across function boundaries.

The remaining planned capture modes are `lend` and `own`. Raw-pointer
parameters, results, and copy captures require an `unsafe` literal. Future
`lend` closures cannot escape. `own` transfers cleanup into the closure, but a
reusable closure cannot consume that capture from its body. `once func` stays
on the future radar.

## Implementation sequence

1. **Callable type foundation — implemented by PR 1.** Named
   signatures, structural type checking, captureless named-function values,
   move-only transfer, indirect calls, ownership/borrow contracts, unsafe
   propagation, and a uniform `{invoke, environment, destroy}` compiler ABI.
2. **Captureless anonymous functions — implemented.** Explicit `[](...)` and
   simplified `(...)` literals, complete contextual signature inference, and
   rejection of partial forms and local captures.
3. **Closure environments and `copy` — implemented.** Heap environments,
   invoke and destroy thunks, snapshot captures, nested and returned closures,
   move-only ownership transfer, and deterministic reverse-order cleanup.
4. **Checked-reference captures — implemented.** `&` and `&const`,
   explicit-capture-only name resolution, last-use borrow liveness, callable
   return-origin propagation, and storage/return/container escape diagnostics.
5. **Raw-resource captures.** Add unsafe `lend` and `own`, nonescape checking,
   allocation-obligation transfer, and exactly-once cleanup.
6. **Integration and release hardening.** Standard-library callbacks,
   examples, editor support, sanitizer/leak/failure-injection coverage, package
   validation, and indirect-call benchmarks.

The uniform callable ABI stores `{invoke, environment, destroy}`. A captureless
value keeps the latter two pointers null. A capturing value stores a heap
environment and typed destroy thunk without changing callable storage layout.
The environment initializes captures in source order. Copy fields own their
snapshots and are released in reverse order. Checked-reference fields are
non-owning and are never destroyed; the borrow checker proves their storage is
live. The destroy thunk then releases the environment allocation.

## Language tests (implemented before 0.2.0)

Both the compiler and Longbow will provide a test command:

```text
flecha test ... [--verbose]
longbow test [--verbose]
```

### Test modules

A test source file is named `<filename>_test.fl` and declares
`module <filename>_test;`. It pairs with exactly one production module: the
sibling `<filename>.fl` file in the same directory. The production module must
exist. Neither a same-named module in another directory nor another module in
the same package can become the pair.

For example, these files form one pair:

```text
src/math.fl
src/math_test.fl
```

Every `_test.fl` file is a test-only module. Test modules are loaded by the test
command and are not lowered or linked by ordinary build and run commands. A
runtime module cannot import a test module. Directly supplying a test module to
a non-test compiler command produces a diagnostic directing the user to the
test command.

Pairing grants visibility but does not create an implicit import. The test
module must explicitly import both its production module and the standard
testing module, and it uses ordinary qualified names:

```flecha
module math_test;

@imports
    :: root::math
    :: lib::std::tests::testing;

test addition(&testing::UnitTest t) {
    result := math::add(20, 22);
}
```

The standard testing API lives in the `std::tests::testing` module, imported as
`:: lib::std::tests::testing`. Its test context type is the exported
`testing::UnitTest` structure.

### Test declarations

The initial declaration form is:

```text
test name(&testing::UnitTest parameterName) {
    body
}
```

The `test` keyword identifies the declaration, so the test name itself does not
need to start with `test`. A test declaration:

- appears only at module scope in a `_test.fl` module;
- has exactly one parameter, whose type is mutable
  `&testing::UnitTest`;
- has no explicit return type and behaves as a `void` function;
- is not generic and has no function modifiers; and
- cannot be a structure member.

The runner supplies the mutable `testing::UnitTest` reference and discovers
every valid `test` declaration in a loaded test module. Ordinary helper functions may also
be declared in the test module, but the runner does not discover or invoke
them. Additional injected parameters are postponed beyond the initial design.

## Test visibility

Test visibility is paired by basename and directory. A `<filename>_test.fl`
module and all helpers declared within it may access all declarations and
members, including private ones, from the sibling `<filename>.fl` module. For
example:

```text
math.fl       <- private declarations
math_test.fl  <- privileged access to math.fl only
```

The privilege does not extend to modules in another directory, unrelated
modules in the same package, or dependency packages. Those retain normal
visibility. Tests that exercise a package through its public API use ordinary
imports and visibility rules. Privileged access cannot be re-exported or passed
through another module, and the production module receives no corresponding
access to declarations in its test module.

The compiler diagnoses:

- a `test` declaration outside a `_test.fl` module;
- a test filename and module-name mismatch;
- a test module without its same-directory production sibling;
- an invalid test signature;
- a runtime import of a test module; and
- private access outside the exact paired-module relationship.

`flecha test <file>` runs one test file, and `longbow test` discovers `_test.fl`
files recursively under the project's `src` directory. By default each file
prints one dot-leader `ok` or `fail` line. `--verbose` prints each test's status
while assertion failures print their assertion, message, and the test
declaration's `file:line:column` beneath the failed status line. Exact assertion
call-site injection and production-code stack traces remain later work. Each
run ends with an `OK [passed/total]` or `FAIL [passed/total]` summary. Filtering
and parallel execution remain later runner work.
