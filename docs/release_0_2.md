# Flecha 0.2.0 milestone

Flecha 0.2.0 is the next language and tooling feature milestone. Items in this
document are design targets, not descriptions of behavior implemented by the
current compiler.

## Function values and anonymous functions

- `func` is reserved for anonymous functions.
- Function values may be accepted as parameters.
- The proposed function-typed parameter spelling is:

```flecha
apply(func(&self, i32)(&i32 <- self) callback, i32 val)
```

The complete rules for captures, ownership, borrow contracts, indirect calls,
and native function pointers remain to be designed.

## Language tests

Both the compiler and Longbow will provide a test command:

```text
flecha test ...
longbow test
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

The runner supplies the mutable `testing::UnitTest` reference and discovers every valid
`test` declaration in a loaded test module. Ordinary helper functions may also
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

`flecha test <file>`, and `longbow test` discovers `_test.fl` files recursively
under the project's `src` directory. Filtering and parallel execution remain
later runner work.
