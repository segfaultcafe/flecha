# Longbow

Longbow is Flecha's project tool. The initial binary creates program and library
projects, checks/builds local source, and runs executables. It delegates language
semantics and import resolution to `flecha`.

Longbow and the compiler are versioned together. `longbow --version` prints the
same release or Git-derived development version as `flecha --version`. Release
packages ship both tools; keeping them together is the supported setup on
Linux, Windows, and macOS. See the [compiler guide](compiler.md) for the
development-version format.

Longbow can move to its own repository and release cycle when Flecha supports
self-hosting and Longbow can be implemented directly in Flecha. Until then,
the shared release version identifies the tested toolchain snapshot.

Build both tools with `make build`. They are produced as `build/longbow` and
`build/flecha`. Keep them together, put `flecha` on PATH, or set `FLECHA_COMPILER`
to the compiler executable. This bootstrap implementation is C++; moving Longbow
to Flecha remains a later self-hosting milestone.

```sh
/path/to/longbow new hello
cd hello
/path/to/longbow check
/path/to/longbow run
```

The program prints `Hello from Flecha!` and exits successfully. Project layout:

```text
hello/
    longbow.toml
    .gitignore
    src/
        main.fl
```

`main.fl` declares `module main;`. The generated manifest is:

```toml
[package]
name = "hello"
version = "0.1.0"
kind = "program"

[dependencies]
```

Publishable libraries also set `registry = "owner/package"` in `[package]`.
That owner-scoped identity is metadata; imports still use the consumer's local
dependency alias and the library's Flecha module names.

Project names may contain letters, digits, spaces, hyphens, underscores, and
dots. They must begin with a letter or digit. Longbow normalizes a library
project's name into a lowercase Flecha identifier for its entry filename and
module declaration, so `math-tools` uses `src/math_tools.fl` and declares
`module math_tools;`.

| Command | Behavior |
| --- | --- |
| `longbow new NAME` | Create a new directory and program scaffold |
| `longbow new NAME --lib` | Create a library with an exported example function |
| `longbow init [--lib]` | Scaffold the current existing directory without overwriting source or a manifest |
| `longbow add OWNER/PACKAGE@REQUIREMENT [--as ALIAS]` | Add an owner-scoped registry dependency to the manifest |
| `longbow remove ALIAS` | Remove one dependency from the manifest |
| `longbow fetch [PROJECT]` | Resolve, verify, cache, and lock dependencies without compiling |
| `longbow update [ALIAS]` | Re-resolve all registry dependencies, or one selected alias, outside its previous lock |
| `longbow publish [PROJECT] --dry-run` | Validate and list the normalized source archive without uploading |
| `longbow publish [PROJECT]` | Publish the immutable version using `LONGBOW_TOKEN` |
| `longbow yank VERSION` | Prevent new selection of a published version while preserving locked builds |
| `longbow check [PROJECT]` | Load and check source and its reachable imports without emitting artifacts |
| `longbow build [PROJECT]` | Compile a program, or emit a host-native object for a library |
| `longbow run [PROJECT] -- ARGS` | Build, then run the program and propagate its exit status |
| `longbow test [PROJECT] [--verbose]` | Discover every source file declaring a `<name>_test` module, run each file's tests, and optionally show individual results |
| `longbow clean [PROJECT]` | Remove the project's generated `target` directory |

Without `PROJECT`, commands locate the nearest ancestor `longbow.toml`, so they
also work inside `src`. A supplied project directory is used directly, including
paths such as `test-me` and `"hi mom"`. Artifacts go to `target`. Failed compilation prevents `run` from executing
an older binary. Arguments are passed directly without a shell. The current
language entry-point signatures do not yet expose command-line arguments to
Flecha code. Existing `.gitignore` files are preserved by `init`.

Registry traffic uses `https://registry.flecha-lang.org/v1` by default. Set
`LONGBOW_REGISTRY` to select a development endpoint and `LONGBOW_TOKEN` for
publish/yank authorization. HTTPS requests use the platform `curl` executable;
bearer tokens are passed through an owner-only temporary header file rather
than command-line arguments. Registry access remains entirely in Longbow.

`longbow add OWNER/PACKAGE@latest` is shorthand for the stable-version `*`
requirement. Longbow stores `*` in the manifest, selects the newest active
non-prerelease version during resolution, and records the exact version and
checksum in `longbow.lock`.

Since Flecha 0.2.1, test discovery reads module declarations rather than file
suffixes. Several arbitrarily named files may contribute to one logical test
module. Longbow invokes every such source file, while each compiler invocation
loads the complete logical module and runs only the tests declared in its entry
file.

## Library projects and local dependencies

`longbow new numbers --lib` generates `src/numbers.fl`, declaring `module numbers;`,
and sets `kind = "library"`. Its entry module is named after the package rather
than `lib.fl`, because `lib` is reserved in import paths. A library may contain
additional modules in its source tree.

An application can configure it with:

```toml
[dependencies]
numbers = { path = "../numbers" }
```

Paths are relative to the application's manifest and identify the dependency
project directory. Longbow reads path dependency manifests recursively and
generates `target/package-graph.json`. Each package receives its own dependency
alias map, so two packages can reuse the same alias for different dependencies.
Source code imports a particular module:

```flecha
@imports
    :: lib::numbers::numbers;
```

The call is `numbers::answer()`. An alias can shorten or distinguish this local
namespace. The standard library bundled beside an installed Longbow executable
is injected into every package as `lib::std`. A root `std` entry in
`[dependencies]` overrides that bundled copy for the entire graph, which is
useful while developing the standard library. Nested packages cannot replace
it. `FLECHA_STANDARD_LIBRARY` can also name a standard-library source directory
for build-tree and custom-toolchain use; an explicit root manifest entry still
takes precedence.

The manifest reader validates SemVer package versions, Flecha toolchain ranges,
path dependencies, pinned Git dependency declarations, registry owner/package
requirements, and registry dependencies with development path overrides.
Unsupported TOML forms, unknown keys, duplicate entries, invalid source
combinations, malformed versions, registry attempts to replace `std`, and path
dependency cycles are diagnosed. Recursive local paths, pinned Git commits, and
registry dependencies with path overrides build now. Git packages are exported
as source blobs without executing checkout hooks, filters, submodules, or
package code. Floating Git branches and tags are rejected. Registry
declarations without an override are acquired by the registry resolver layer.
Prebuilt library objects are not used to resolve imports.

Resolution writes a deterministic `longbow.lock` beside the manifest. It
records graph node identities, exact package versions, source kinds and paths,
full Git revisions, package-local dependency edges, Flecha compatibility
ranges, and SHA-256 content checksums. The root workspace is intentionally not
content-locked so normal application edits do not invalidate dependency state.
Toolchain `std` is recorded by source kind rather than a host-specific
installation path, keeping the lock portable across installed toolchains.

Verified source trees are stored by SHA-256 under the platform-native user
cache directory. Set `LONGBOW_CACHE_DIR` to use an explicit cache root in CI or
managed installations. Cache entries contain a completion marker and are
rehash-verified before reuse; incomplete or corrupt entries are rejected. The
cache is disposable and contains no authoritative project state.

A runnable multi-module example is in `examples/modules`. From that directory,
run `../../build/longbow run` after building the tools.

## Native link inputs

Programs using `extern` declarations can attach prebuilt object files or static
libraries in a native manifest section:

```toml
[native]
bridge = { path = "native/bridge.o" }
```

Paths are relative to `longbow.toml`. Longbow forwards every native input to
the final system-linker invocation for `build` and `run`. It does not compile C
or C++ sources; build those inputs with the platform toolchain first. Library
packages emit only a Flecha object, so an application consuming such a library
must list the corresponding native input in its own manifest.
