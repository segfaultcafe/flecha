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

Raw-resource captures use `lend name` and `own name`, and require an `unsafe`
literal. `lend` preserves the current owner's cleanup obligation, prevents
conflicting free or transfer while the closure is live, and cannot escape its
creation scope. `own` transfers cleanup into the closure environment, whose
destroy thunk releases it exactly once. A reusable closure may read or mutate
through an owned pointer but cannot consume it from its body. Generalized
owning captures and single-use callables are specified separately below.

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
5. **Raw-resource captures — implemented.** Unsafe `lend` and `own`, lexical
   nonescape checking, allocation-obligation transfer, conflict diagnostics,
   and exactly-once cleanup.
6. **Integration and release hardening — implemented.** Cross-module
   callbacks, aggregate fields, methods, generic transfers, runnable examples,
   editor coverage, leak-sanitized ownership regressions, and packaged-compiler
   ABI validation on every release platform. Indirect-call benchmarks remain a
   performance follow-up rather than a release gate.
7. **Quiver visibility — implemented.** Module declarations distinguish
   global `export`, directory-and-root-scoped `quiver`, and unmarked
   module-private reach. Import binding, generic expansion, nominal
   specialization boundaries, API exposure checks, exact-owner member privacy,
   diagnostics, and editor grammars enforce the same model.

The uniform callable ABI stores `{invoke, environment, destroy}`. A captureless
value keeps the latter two pointers null. A capturing value stores a heap
environment and typed destroy thunk without changing callable storage layout.
The environment initializes captures in source order. Copy fields own their
snapshots and are released in reverse order. Checked-reference fields are
non-owning and are never destroyed; the borrow checker proves their storage is
live. Lent raw resources are also non-owning. Owned raw resources are released
in reverse capture order before the destroy thunk releases the environment
allocation.

The integration fixture exercises captureless and capturing callables through
module exports, generic functions, structure fields and methods, tuples, fixed
arrays, checked-reference captures, and owned raw-resource captures. Sanitizer
runs keep leak detection enabled for both compiler unit tests and native
programs. Release and clean-install jobs run the same callable ABI smoke on
Linux x86-64, Windows x86-64, macOS x86-64, and macOS arm64.

## Expanded v0.2.0 requirements

The implemented callable foundation is necessary but is not the complete
v0.2.0 feature boundary. The release also requires the following four stages,
in dependency order.

7. **Generalized owning captures — implemented
   ([#64](https://github.com/segfaultcafe/flecha/issues/64)).** `own` must
   transfer any movable, noncopyable safe value into a closure environment,
   including callable values and aggregates. The captured value keeps its
   checked-reference origins, becomes unavailable at the creation site, and is
   destroyed exactly once with the environment. Immovable values are rejected.
   Safe owning captures do not make a literal unsafe; owning a raw resource
   continues to require an `unsafe` literal. A reusable closure can use an
   owned capture but cannot consume it from its body.

   ```flecha
   Mapper decorated = [own mapper](i32 value) => (i32) {
       return mapper(value) + 1;
   };
   ```

8. **Generic callable signatures — implemented
   ([#66](https://github.com/segfaultcafe/flecha/issues/66)).** Callable aliases
   may declare type parameters. A specialization is an ordinary structural
   callable signature, with parameter and result substitution plus the same
   safety, ownership, and borrow-contract checks as a nongeneric alias. A
   specialized alias is valid everywhere an existing callable type is valid,
   including contextual anonymous-function inference. Missing, extra, or
   unresolved type arguments receive direct diagnostics.

   ```flecha
   func Predicate<T>(&const T) => (bool);
   func Transform<T, U>(T) => (U);
   ```

9. **Standard-library callback algorithms — required
   ([#68](https://github.com/segfaultcafe/flecha/issues/68)).** The standard
   library must provide `for_each`, `find_if`, and `fold` over slices. These
   APIs accept named functions, captureless anonymous functions, and capturing
   closures through generic callable signatures. Their callable ownership,
   element and accumulator constraints, safety propagation, and borrow
   contracts must be explicit. End-to-end tests and runnable examples cover
   every callable form.

10. **Deferred indirect and member calls — required
    ([#65](https://github.com/segfaultcafe/flecha/issues/65)).** `defer` must
    accept calls through callable values and methods. The callee, receiver, and
    arguments follow one documented registration-time evaluation rule, remain
    valid until scope exit, and execute in the existing LIFO order. Ownership,
    destruction, safety, and checked-reference behavior must hold across normal
    exit, early return, and nested scopes.

    ```flecha
    defer callback(value);
    defer writer.flush();
    ```

## Stretch boundary

Single-use `once func` callables are tracked by
[#67](https://github.com/segfaultcafe/flecha/issues/67) as a non-blocking
v0.2.0 stretch item. Invoking such a value would consume it, allowing its body
to consume owned captures and making a second invocation a move error. Its type
identity, compatibility rules, ABI behavior, and never-invoked destruction
must be settled before implementation. It may land in v0.2.0 when complete and
stable, but it must not delay the release. Indirect-call benchmarks remain a
non-blocking performance follow-up as well.

## v0.2.0 release condition

The release is ready only when stages 1 through 10 are complete, their language
and end-to-end tests pass under the sanitizer and packaged-compiler matrix, and
the standard-library, editor, examples, and diagnostics agree with the final
syntax and semantics. The `once func` stretch item and indirect-call benchmark
are explicitly not release gates.

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
module and all helpers declared within it may access module-private top-level
declarations from the sibling `<filename>.fl` module. This privilege never
bypasses private structure or enum members, which remain accessible only to
methods of their exact owning type. For example:

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
