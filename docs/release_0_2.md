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

Nonempty capture lists are reserved for the closure phases. Their planned
shape is `[copy factor, &counter, &const limit](...) => (...) { ... }`.

Capture modes are `copy`, `&`, `&const`, `lend`, and `own`. Raw-pointer
parameters, results, and captures require an `unsafe` literal. `lend` closures cannot
escape. `own` transfers cleanup into the closure, but a reusable closure cannot
consume that capture from its body. `once func` stays on the future radar.

## Implementation sequence

1. **Callable type foundation — implemented by PR 1.** Named
   signatures, structural type checking, captureless named-function values,
   move-only transfer, indirect calls, ownership/borrow contracts, unsafe
   propagation, and a uniform `{invoke, environment, destroy}` compiler ABI.
2. **Captureless anonymous functions — implemented.** Explicit `[](...)` and
   simplified `(...)` literals, complete contextual signature inference, and
   rejection of partial forms and local captures.
3. **Closure environments and `copy`.** Heap environments, invoke and destroy
   thunks, snapshot captures, and deterministic reverse-order cleanup.
4. **Checked-reference captures.** Add `&` and `&const`, explicit-capture-only
   name resolution, borrow liveness, and escape diagnostics.
5. **Raw-resource captures.** Add unsafe `lend` and `own`, nonescape checking,
   allocation-obligation transfer, and exactly-once cleanup.
6. **Integration and release hardening.** Standard-library callbacks,
   examples, editor support, sanitizer/leak/failure-injection coverage, package
   validation, and indirect-call benchmarks.

The uniform callable ABI is fixed before capture environments arrive. A
captureless value stores an adapter in `invoke` and null environment/destructor
pointers. Later phases populate the latter two fields without changing user
code or callable storage layout.

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
