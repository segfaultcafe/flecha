# Flecha

<p align="center">
  <img src="identity/flecha-logo.png" alt="Flecha logo" width="360">
</p>

<p align="center">
  <a href="https://flecha-lang.org/">Website</a> ·
  <a href="https://flecha-lang.org/docs/">Documentation</a> ·
  <a href="https://flecha-lang.org/install/">Install</a> ·
  <a href="https://flecha-lang.org/examples/">Examples</a>
</p>

Flecha is an early-stage, statically typed systems programming language and compiler. It is designed for predictable execution, explicit memory operations, strong diagnostics, and direct access to native targets without a garbage-collected runtime.

The compiler is implemented in modern C++ and currently includes a tokenizer, parser, semantic analyzer, lowering pipeline, LLVM backend, native compiler, runtime support, standard library, and the Longbow package/project tool.

> **Status:** Flecha is under active development. The language specification and several design documents describe intended or experimental behavior; they are not yet a promise of a stable language or ABI.

## Highlights

- Statically typed primitives, functions, enums with exhaustive matching, tuples, structures, arrays, strings, and error values
- Explicit `unsafe` features for raw pointers and allocation
- Checked references and slices, ownership contracts, and definite-assignment analysis
- Structural callable types, closures with explicit capture modes, and
  consuming `once func` values
- Modules and library imports through `root::` and `lib::` paths, with global,
  quiver, and module-private visibility
- First-class `flecha test` and `longbow test` workflows
- Generic standard-library callback algorithms and safe owning arrays
- LLVM IR emission and native executable/object generation
- Human-readable source diagnostics
- A small package/project workflow through `longbow`
- Visual Studio Code and Neovim syntax support

## Quick start

### Requirements

Build requirements are:

- CMake 3.20 or newer
- A supported C++23 compiler: Clang 17-23, GCC 13-16, or Apple Clang 16-17
- Ninja, or another CMake generator
- LLVM 17 through 23 development libraries and tools

LLVM's `lli` is required for the `llvm-poc` IR smoke target. Neovim is
required only for the editor integration test. Test builds fetch GoogleTest
through CMake unless a local checkout is supplied.
The pinned platform packages, linkers, SDKs, and release baselines are listed
in [`docs/toolchains.md`](docs/toolchains.md).

### Build the compiler

```sh
make build
```

The main compiler is written to `build/flecha`; the project tool is written to `build/longbow`.

To configure and build directly with CMake:

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=OFF
cmake --build build --parallel
```

Create relocatable `.tar.gz` and `.zip` release archives with:

```sh
cmake --build build --target package
```

The archives contain the Flecha compiler, Longbow, their required shared
runtime dependencies, the Flecha runtime and standard library, reference
documentation, and Visual Studio Code and Neovim support.
See [`docs/release_0_1.md`](docs/release_0_1.md) for the Linux, Windows, and
macOS release gates.

### Docker sandbox

Build a disposable container from the current working tree and enter it with:

```sh
make sandbox
make enter-sandbox
```

The sandbox provides the installed Flecha compiler and Longbow, a native
linker, Neovim with Flecha support, and a starter project. See the
[`Docker sandbox guide`](docs/sandbox.md) for lifecycle and configuration
details.

### Install editor support

Each GitHub Release provides `flecha-language-VERSION.vsix` for Visual Studio
Code. Install it on Linux, macOS, or Windows with:

```sh
code --install-extension flecha-language-VERSION.vsix
```

Compiler archives contain the Neovim plugin under
`share/flecha/editors/neovim`. See the
[`editor installation guide`](docs/editors.md) for Neovim commands, checksums,
upgrades, uninstalling, and packaging the VS Code extension from a checkout.

### Check and build a program

Flecha source files use the `.fl` extension. An executable entry module must define either `i32 main()` or `void main()`.

```sh
build/flecha check examples/factorial.fl
build/flecha build examples/factorial.fl -o build/factorial
./build/factorial
```

The example prints:

```text
Factorial: 120
```

The compiler also supports explicit output stages:

```sh
# Emit textual LLVM IR
build/flecha --emit-llvm examples/llvm_poc.fl -o build/llvm_poc.ll

# Emit a native object file
build/flecha --emit-object examples/llvm_poc.fl -o build/llvm_poc.o
```

The complete command-line form is:

```text
flecha <check|build|test|--emit-llvm|--emit-object> <input.fl> [-o output]
       [--root source-directory] [--lib name=source-directory ...]
       [--package-graph graph.json]
       [--link object-or-library ...]
```

## A small Flecha program

```flecha
i32 factorial(i32 n) {
    if n <= 1 {
        return 1;
    }

    return n * factorial(n - 1);
}

void main() {
    result := factorial(5);
    stdout("Factorial: ");
    stdoutln(result);
}
```

Functions place the return type before the function name. Local type inference uses `:=`; explicit declarations are also supported. See [`SPEC.md`](SPEC.md) for the current syntax and design goals.

## Projects and packages with Longbow

Longbow manages a project described by `longbow.toml`.

```sh
# Create a program project
build/longbow new hello
cd hello

# Check, build, test, or run it from the project directory
../build/longbow check
../build/longbow build
../build/longbow test
../build/longbow run
```

Create a library with `longbow new NAME --lib`, or initialize an existing empty project directory with `longbow init` (and `--lib` for a library). A minimal manifest looks like this:

```toml
[package]
name = "my_program"
version = "0.1.0"
kind = "program"

[dependencies]
```

Dependencies may be recursive local paths, exact Git commits, or owner-scoped
public registry packages. Imports use the local alias on the left, never the
canonical registry identity:

```toml
[dependencies]
math = { path = "../math" }
codec = { git = "https://example.com/team/codec.git", rev = "0123456789abcdef0123456789abcdef01234567" }
json = { package = "flecha/json", version = "^1.2" }
```

Longbow resolves these sources into a package-local alias graph, writes
`longbow.lock`, and verifies shared cache entries before reuse. The compiler
receives the resolved graph and never performs network access.

Longbow's available commands are:

```text
longbow new NAME [--lib]
longbow init [--lib]
longbow add OWNER/PACKAGE@REQUIREMENT [--as ALIAS]
longbow remove ALIAS
longbow fetch [PROJECT]
longbow update [ALIAS]
longbow login
longbow logout
longbow whoami
longbow publish [PROJECT] [--dry-run]
longbow yank VERSION
longbow check|build|run|clean [PROJECT]
longbow test [PROJECT] [--verbose]
longbow run -- [program arguments]
```

`longbow add OWNER/PACKAGE@latest` selects the newest active stable release;
the manifest stores the equivalent `*` requirement and the lockfile records the
exact resolved version.

See [`docs/longbow.md`](docs/longbow.md) for the complete project workflow and
[`docs/package_ecosystem.md`](docs/package_ecosystem.md) for identities, locks,
cache behavior, archives, and the registry protocol.

## Testing

Configure and run the complete test suite with:

```sh
make test
```

Useful focused targets include:

```sh
make test-semantic       # Semantic and scope tests
make test-neovim         # Neovim syntax/filetype integration
make package-vscode      # Build and verify the installable VSIX
make test-sanitizers     # AddressSanitizer and UBSan tests
make llvm-poc            # Emit and execute LLVM IR; expects exit code 42
make native-poc          # Build and execute a native example; expects 42
make native-if-also
make native-functions
make native-callables
build/flecha build examples/callback_algorithms.fl --lib std=std/src \
    -o build/callback-algorithms
make native-while
make native-loop-control
make native-casts
make native-string-view
make native-owning-string
make native-stdout
make native-definite-assignment
```

Test builds use `build-tests` by default. The sanitizer build uses `build-sanitizers`. These locations can be overridden:

```sh
make BUILD_DIR=out TEST_BUILD_DIR=out-tests build
```

If GoogleTest is already available locally, pass its source directory to the sanitizer target with `GTEST_SOURCE_DIR=/path/to/googletest`.

## Repository layout

```text
src/                    Compiler, runtime, standard library, and tests
std/                    Flecha standard-library source packages
examples/               Small language and package examples
docs/                   Feature, implementation, and design documentation
editors/neovim/         Neovim filetype and syntax support
editors/vscode/         Visual Studio Code syntax extension
SPEC.md                 Current language specification draft
CODE_STYLE.md           C++ and repository style guidance
CMakeLists.txt          CMake build definition
Makefile                Convenient build, test, and example targets
```

## Documentation guide

- [`docs/vision.md`](docs/vision.md) — the direction toward a self-hosted,
  performant Flecha 1.0 and the possibilities beyond it
- [`CHANGELOG.md`](CHANGELOG.md) — release notes and upgrade-relevant changes
- [`SPEC.md`](SPEC.md) — language goals, syntax, types, and current specification draft
- [`docs/compiler.md`](docs/compiler.md) — compiler commands, outputs, and installed layout
- [`docs/longbow.md`](docs/longbow.md) — project manifests, commands, and local dependencies
- [`docs/sandbox.md`](docs/sandbox.md) — disposable Docker development environment
- [`docs/editors.md`](docs/editors.md) — VS Code and Neovim installation and packaging
- [`docs/release_0_1.md`](docs/release_0_1.md) — 0.1 distribution contract and release gates
- [`docs/release_0_1_1.md`](docs/release_0_1_1.md) — v0.1.1 stabilization scope and release condition
- [`docs/release_0_2.md`](docs/release_0_2.md) — v0.2.0 language and tooling design targets
- [`docs/release_0_2_1.md`](docs/release_0_2_1.md) — v0.2.1 module-correctness milestone
- [`docs/release_0_3.md`](docs/release_0_3.md) — v0.3.0 application-capable standard-library milestone
- [`docs/toolchains.md`](docs/toolchains.md) — supported compiler, LLVM, linker, SDK, and CI versions
- [`docs/semantic_analysis.md`](docs/semantic_analysis.md) — name binding, types, scopes, and analysis pipeline
- [`docs/llvm_codegen.md`](docs/llvm_codegen.md) — current LLVM backend scope and limitations
- [`docs/modules.md`](docs/modules.md) — current modules, imports, and package libraries
- [`docs/module_composition.md`](docs/module_composition.md) — corrected 0.2.1 logical-module model
- [`docs/memory_model.md`](docs/memory_model.md) — memory model proposal and implementation status
- [`docs/ownership_contracts.md`](docs/ownership_contracts.md) — `own` and `lend` contracts
- [`docs/checked_references.md`](docs/checked_references.md) — checked reference behavior
- [`docs/ref_and_slice_returns.md`](docs/ref_and_slice_returns.md) — returned reference and slice contracts
- [`docs/dynamic_arrays.md`](docs/dynamic_arrays.md) and [`docs/fixed_arrays.md`](docs/fixed_arrays.md) — array features
- [`docs/collections.md`](docs/collections.md) — Stage 2 collection ownership,
  relocation, borrowing, and acceptance contracts
- [`docs/memory.md`](docs/memory.md) — typed slice copying, filling, swapping, and error contracts
- [`docs/math.md`](docs/math.md) — standard-library ordered math operations
- [`docs/bytes.md`](docs/bytes.md) — raw byte storage and sequence operations
- [`docs/encoding.md`](docs/encoding.md) — binary and hexadecimal encoding
- [`docs/hashing.md`](docs/hashing.md) — structural non-cryptographic hashing
- [`docs/error_values.md`](docs/error_values.md) — error declarations and propagation
- [`docs/enums.md`](docs/enums.md) — nominal enums, defaults, discriminants, and member functions
- [`editors/vscode/README.md`](editors/vscode/README.md) — Visual Studio Code highlighting setup
- [`editors/neovim/README.md`](editors/neovim/README.md) — Neovim installation and usage

The documentation distinguishes implemented behavior from proposals and
current limitations. When behavior differs between a design proposal and the
compiler, the feature guide, compiler tests, and diagnostics describe the
implemented contract.

## Development

Use the existing build and test targets when possible. C++ code follows the conventions in [`CODE_STYLE.md`](CODE_STYLE.md), and generated build directories should remain uncommitted. Before opening a change, run the relevant focused tests as well as `make test` when practical.

Pull requests run unit and native tests, sanitizers, editor checks, VSIX
packaging, and cross-platform package smoke tests. Publishing a GitHub Release
with a matching semantic version tag builds and attaches the
Linux, Windows, and macOS bundles plus the Visual Studio Code extension. See the
[`0.1 release automation guide`](docs/release_0_1.md#automation).

The working tree may contain experimental language work, so changes should preserve existing diagnostics, examples, and test fixtures unless the behavior change is intentional and documented.

## Contributing

Contributions are welcome. Please read [`CONTRIBUTING.md`](CONTRIBUTING.md)
before opening a pull request, including the project's human-authorship and
AI-assisted contribution policy. C++ changes also follow
[`CODE_STYLE.md`](CODE_STYLE.md).

## License

Flecha is distributed under the terms of [`LICENSE`](LICENSE).
