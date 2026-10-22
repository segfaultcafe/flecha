# Changelog

## Unreleased

## 0.2.3 - 2026-10-22

Flecha 0.2.3 fixes nested receiver reborrowing in method-call arguments.

### Fixed

- Mutable method receiver borrows now activate after argument evaluation,
  allowing temporary shared reborrows that produce ordinary values, such as
  `self->read(self->remaining())`.
- Receiver reservation still rejects overlapping mutation, moves, and
  arguments that retain a borrow into the mutable call.

## 0.2.2 - 2026-10-21

Flecha 0.2.2 fixes zero-state classification for structures containing
move-only runtime-array owners.

### Fixed

- Null `*[]T` owning buffers now count as trivially zeroable when classifying
  structure `zero()` implementations and generated initializers.
- Structures with an equivalent trivial zero state can use `none` directly,
  including when nested in tuple results, without weakening move-only
  ownership.

## 0.2.1 - 2026-10-20

Flecha 0.2.1 corrects module identity and composition so modules are logical
scopes assembled from declarations rather than being coupled to filenames.

### Fixed and improved

- Multiple source units with the same declared module name and namespace now
  form one logical module, share one declaration scope and nominal identity,
  and may access each other's module declarations in either direction.
- Imports remain local to the source unit that declares them. Import paths,
  brace selections, and wildcards resolve declared module names instead of
  filenames, and repeated imports of one canonical module are deduplicated.
- Distinct same-named modules can overlap under one implicit import namespace.
  Unique members resolve normally, collisions are diagnosed at their use site,
  and explicit aliases keep each origin isolated.
- `private module` remains compatible but is deprecated and must be consistent
  across every source unit in a logical module. `export` now always classifies
  a declaration as public, while the legacy private-module import gate remains.
- Test pairing and Longbow discovery now use declared `<name>_test` module
  identity, allowing arbitrarily named files and multiple source units to
  contribute tests to one generated harness.

## 0.2.0 - 2026-10-20

Flecha 0.2.0 introduces callable values and closures with explicit ownership,
generic and single-use callable signatures, first-class language tests, scoped
module visibility, and broader standard-library containers and I/O support.

### Language

- Added single-use `once func` callable types and `once` anonymous functions.
  Invocation consumes the callable, reusable callables can move into a
  compatible once slot, and once bodies may consume owned captures without
  double destruction.
- `defer` now accepts callable values and structure methods in addition to
  direct functions. Indirect callees are moved into the deferred record, method
  receivers remain safely borrowed through scope exit, and all forms preserve
  registration-time evaluation, LIFO execution, and exactly-once teardown.
- Modules may now be declared with `private module name;`, restricting imports
  to the same quiver. Within a private module, `export` exposes a declaration to
  that quiver rather than globally; wildcard imports and separate source or
  library identities preserve the boundary.
- Top-level declarations now support `quiver` visibility for sibling modules in
  the same immediate directory and source-root or library identity. Imports,
  wildcard directory imports, generics, API exposure validation, and generated
  nominal specializations preserve the boundary; private members remain limited
  to methods of their exact owning type.
- Callable signatures may now declare type parameters, for example
  `func Mapper<T>(T) => (T);`. Explicit specializations work across modules,
  aggregates, containers, and other generic declarations while preserving
  structural compatibility, contextual anonymous-function inference, safety,
  ownership, and borrowed-result contracts.
- `own` captures now move callable values and movable noncopyable safe
  aggregates into closure environments. Nested closures and callable
  containers retain borrow origins and run recursive teardown exactly once;
  copyable and immovable captures receive direct diagnostics.
- Callable values now transfer correctly through structures, tuples, fixed
  arrays, methods, modules, and generic functions without duplicating closure
  ownership.
- Unsafe anonymous functions can capture tracked raw resources with `lend` or
  `own`. Lending closures cannot escape or overlap a free/transfer, while owned
  captures move cleanup into the closure and release exactly once when its
  environment is destroyed.
- Anonymous functions can explicitly borrow outer storage with `&name` or
  `&const name`. Callable moves, returns, and containers preserve borrow
  origins, and invalid storage or return escapes are diagnosed.
- Checked reference and slice results now declare their source parameter with
  `<-`, for example `&const i32 view(&const i32 value) <- value`.
- Borrowed-result contracts compose through methods, modules, generics, and
  transitive calls. Flat tuples may return multiple borrowed values when they
  are immediately destructured; overlapping mutable results are rejected.
- Short tuple declarations now reuse writable variables from the same lexical
  scope, so `value, err := call()` follows Go-style mixed declaration and
  assignment semantics. At least one non-blank destination must be new.
- Runtime-sized `new<[]T>` allocations now return move-only `*[]T` owners that
  carry their length, eliminating allocator metadata queries from owner
  indexing and slicing while retaining a consuming compatibility conversion to
  explicitly typed raw `*T` APIs.

### Performance

- Native object emission now runs LLVM's target-aware default `O2` pipeline.
- Fresh owning-string results, including concatenation and scalar formatting,
  transfer into their first owning destination instead of being cloned.

### Tooling

- Added first-class `flecha test` and `longbow test` commands, paired
  `_test.fl` modules, strict `test` declaration validation, production-module
  private access, verbose reporting, and standard testing/assertion modules.
- Sanitizer validation now keeps leak detection enabled for compiler and native
  callable tests, and release packages execute a cross-module callable ABI
  smoke covering reusable and single-use closures plus deferred callable and
  member invocation on every supported platform.
- Runnable callable, callback-algorithm, deferred-call, and single-use callable
  examples cover named functions, inferred signatures, every capture mode,
  generic algorithms, and consuming closures.
- Development builds of Flecha and Longbow now include the commit distance,
  abbreviated Git revision, and dirty-worktree state in `--version` output.

### Standard library

- Added generic slice algorithms including `for_each`, `find_if`, `fold`,
  `any_of`, `all_of`, `count_if`, `sort_by`, `reverse`, `fill`, `index_of`, and
  `contains`, together with matching safe-array methods.
- Expanded safe and raw arrays with construction, append, removal, cloning,
  capacity management, views, mutation algorithms, and lifecycle coverage.
- Added file streams, exact and partial reads, string and byte writes, seeking,
  file metadata, and expanded filesystem path operations with structured I/O
  errors.

## 0.1.3 - 2026-10-19

Flecha 0.1.3 is a patch release improving character-byte interoperability.

### Fixed

- Character values convert implicitly to `u8`, and borrowed string data can be
  passed to compatible byte-pointer parameters at the external ABI boundary.

## 0.1.2 - 2026-10-17

Flecha 0.1.2 is a patch release correcting zero-construction triviality.

### Fixed

- Explicit `zero()` overrides that only construct canonical zero values are
  recognized as trivial, including through generic and nested structures.

## 0.1.1 - 2026-10-16

Flecha 0.1.1 is a stabilization release for the first installable toolchain.
It does not introduce a new language-feature wave.

### Fixed and improved

- Release archives are exercised by fresh downstream consumers on Linux
  x86-64, Windows x86-64, macOS x86-64, and macOS arm64. Each test relocates
  the unpacked installation before checking the compiler, native output,
  Longbow workflows, and bundled dependencies.
- Missing or unusable host linkers now produce structured diagnostics with the
  attempted drivers, linker output, and platform-specific prerequisite hints.
- Supported C/C++ compiler and LLVM ranges are enforced consistently by CMake,
  CI, release builds, and documentation. Release jobs also verify the actual
  linker, runtime or SDK, target, and Xcode versions.
- Windows clean-install verification queries the MSYS2 UCRT64 runtime through
  the installed `pacman.exe` path.

### Distribution and documentation

- GitHub Releases now include a validated `flecha-language-0.1.1.vsix` with the
  platform archives and consolidated checksums.
- Visual Studio Code and Neovim installation, upgrade, verification, and
  removal workflows are documented.
- The specification and implementation guides now distinguish shipped 0.1
  behavior from remaining proposals, including the full LLVM backend, checked
  references and slices, allocation analysis, ownership contracts, generics,
  and current ABI and cross-compilation limits.

### Installation requirements

Release archives bundle their matching LLVM runtime and do not require LLVM
development files. Native executable builds still require the documented host
linker and platform SDK prerequisites in
[`docs/toolchains.md`](docs/toolchains.md).
