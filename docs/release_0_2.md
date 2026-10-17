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

Test source files use the `<filename>_test.fl` convention. Every such file is a
test-only module. Test declarations must not be treated as runtime declarations
by ordinary build and run commands.

A test module imports the standard testing package and declares tests with the
`test` introducer:

```flecha
@imports :: lib::std::testing;

test <fn_name>(t testing::UnitTest, ...) {
    // Test happens here.
}
```

The test command discovers and runs declarations beginning with `test`. The
runner supplies the `testing::UnitTest` context. The meaning and permitted
sources of any additional parameters remain to be designed.

## Test visibility

White-box tests need access to private implementation details, but package-wide
unrestricted access would weaken module boundaries and make unrelated tests
depend on each other's internals.

Test visibility is paired by basename and directory. A `<filename>_test.fl`
module may access all declarations and members, including private ones, from
the sibling `<filename>.fl` module in the same directory. For example:

```text
math.fl       <- private declarations
math_test.fl  <- privileged access to math.fl only
```

The privilege does not extend to modules in another directory, unrelated
modules in the same package, or dependency packages. Those retain normal
visibility. Tests that exercise a package through its public API use ordinary
imports and visibility rules.
