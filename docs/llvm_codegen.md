# LLVM code generation

The LLVM backend consumes the lowered `lowering::BoundProgram`; parsing, name
lookup, generic specialization, semantic checks, borrow checking, and allocation
analysis have already completed. The native compiler configures the host target
triple and data layout. `LlvmCodeGenerator` then declares functions before
emitting their bodies and verifies every generated module with LLVM.
Before object emission, the native compiler runs LLVM's target-aware default
`O2` pipeline, including scalar simplification, loop optimization, and
vectorization. Textual `--emit-llvm` output remains the direct verified lowering
and is useful for inspecting frontend code generation.

## Implemented scope

The backend emits the language features exercised by the native compiler and
end-to-end test suites:

- primitive integers, floating-point values, Booleans, characters, `none`, and
  null raw pointers;
- functions, recursion, direct calls, extern declarations, locals, assignments,
  numeric conversions, and short-circuit Boolean operations;
- conditional and loop control flow, `also`, `break`, `proceed`, positions, and
  checked jumps;
- strings and string views, including conversions, content comparison, checked
  view operations, output, and deterministic cleanup;
- raw pointers, checked references, fixed arrays, checked slices, tuples, error
  values, enums and exhaustive matches, and structures with lifecycle hooks;
- direct deferred calls and cleanup on the supported scope-exit edges;
- system allocation, runtime-sized arrays, typed destruction, and `free`;
- concrete generic specializations produced before semantic analysis; and
- target-width `isize` and `usize`, C ABI integer extension attributes for
  supported extern calls, object emission, and host-native executable linking.

Aggregate calling conventions are internal to Flecha unless a feature guide
explicitly documents C compatibility. The backend diagnoses any unsupported
lowered node or conversion instead of emitting unverified IR.

## Commands and smoke test

Emit textual LLVM IR with the compiler driver:

```sh
make build
build/flecha --emit-llvm examples/llvm_poc.fl -o build/llvm_poc.ll
```

Run the IR smoke target with:

```sh
make llvm-poc
```

The target emits the example module, executes it with `lli`, and verifies exit
code `42`. Native object and executable generation are documented in
[native code generation](native_codegen.md).

## Current limits

The installed compiler targets its host machine and currently fixes native
object optimization at `O2`. Explicit cross-compilation triples and sysroots,
optimization-level selection, source-level debug
information, a stable general-purpose Flecha ABI, and a C source backend are not
part of the 0.1 implementation. Language feature guides document additional
type- and operation-specific limits before code generation.
