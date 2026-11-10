# Changelog

## Unreleased

### Added

- Flecha now builds and installs `flecha-lsp`, with full-document
  synchronization, diagnostics, hover information, go to definition,
  references, completion, rename, and semantic tokens.

## 0.3.2 - 2026-11-09

Flecha 0.3.2 introduces the compiler-side analyzer protocol used by language
servers and fixes several parser, semantic-analysis, and diagnostic issues.
See the [0.3.2 release notes](docs/release_0_3_2.md) for integration and
upgrade details.

### Added

- The compiler now exposes `flecha analyze --stdio`, a framed JSON protocol
  for versioned document overlays and project-aware diagnostics.
- The Visual Studio Code extension now starts `flecha-lsp` for live compiler
  diagnostics, with configurable server/compiler paths, protocol tracing, and
  a restart command for development builds.
- The Neovim plugin can run the enclosing Longbow project with `<leader>rr`,
  including from an empty editor at the project root, display compiler output
  in quickfix, and open a selected diagnostic at its exact source line with
  `<leader>fe`.
- The Neovim plugin can run verbose Flecha tests for the enclosing Longbow
  project with `<leader>rt` or `:LongbowTest`; failed test declarations are
  navigable from quickfix with `<leader>fe`.

### Fixed

- Allocation analysis now summarizes safe resource-helper calls after
  verifying their bodies once, preventing exponential compile times in code
  that uses layered parsers and other allocation-backed abstractions.
- Borrow origins now propagate recursively through tuple results, preventing
  checked references captured by aggregate values from escaping their scope.
- Missing block braces before top-level declarations now produce a focused
  closing-brace diagnostic instead of reporting the declaration as an invalid
  expression.
- Missing enum and error variants now use stable source-level type names in
  diagnostics instead of internal module-qualified names.

## 0.3.1 - 2026-11-06

Flecha 0.3.1 fixes derived JSON support for slices.

### Fixed

- Generated JSON writers serialize owning slices as arrays, recursively
  including nested slices and decorated structure elements.
- Generated JSON readers deserialize arrays into `own []T`, preserving
  ownership and cleaning up partially decoded elements on parse or allocation
  failure. Borrowed slice destinations now produce a precise compile-time
  diagnostic.

## 0.3.0 - 2026-11-05

Flecha 0.3.0 makes the toolchain application-capable with a substantially
larger standard library, deterministic source-package management, portable
installed-package qualification, and hard release gates for correctness,
stability, and performance. See the
[0.3.0 release and upgrade notes](docs/release_0_3_0.md) for the supported
platforms, migration guidance, and known limitations.

### Added

- Added allocation-free constant-time equality for byte slices and `stringv`
  values plus guaranteed zeroing of mutable byte slices, backed by versioned
  native boundaries that preserve both security-sensitive operations through
  optimization.
- Added allocation-free `std::cryptography::sha2` and
  `std::cryptography::hmac` modules with portable SHA-256, SHA-512,
  HMAC-SHA-256, and HMAC-SHA-512, including one-shot and incremental APIs,
  fixed-size owning outputs, and published conformance vectors.
- Added portable IPv4/IPv6 socket-address values and blocking hostname
  resolution with ordered owning results, explicit ports, stable network
  errors, and a versioned native resolver boundary.
- Added owning blocking UDP sockets with portable IPv4/IPv6 binding,
  connected and destination-addressed datagram operations, sender and peer
  address recovery, explicit truncation and payload-boundary errors,
  independent timeouts, and deterministic teardown.
- Added `std::tests::benchmark` with validated warmup and sample configuration,
  monotonic in-process timing, setup/teardown hooks, integer summary statistics,
  and deterministic versioned JSON output for CI baselines.
- Flecha tests can now own self-cleaning temporary directory fixtures, compare
  text or binary golden files through an explicit update workflow, and generate
  deterministic random test data with reproducible seed reporting.
- Standard testing assertions now include value-rich numeric equality and
  ordering checks, escaped and bounded string diagnostics, byte mismatch
  context, and explicit error-family and error-variant checks.
- Direct generic method calls can infer omitted type arguments from supplied
  arguments and expected results, including known tuple-destructuring
  destinations, with the same rules across exported, quiver, internal, and
  private declarations.
- Added `std::random::secure` for allocation-free operating-system secure bytes
  and `std::random::generator::Generator` for reproducible integers, unbiased
  ranges, unit floats, booleans, byte streams, and shuffling.
- Added `std::time::duration` and `std::time::clock` with checked normalized
  durations, opaque monotonic instants, Unix wall-clock timestamps, and
  interruption-safe blocking sleep over a versioned native time ABI.
- Added `std::io::path` with allocating lexical join and normalization plus
  borrowed filename, parent, and extension views for host POSIX or Windows
  path rules.
- Free functions can declare an allocation-free forwarding pack with
  `values...` and expand it in their single forwarding call with `values...`.
  Forwarding aliases emit no runtime function or variadic ABI, and borrow
  contracts distinguish pack-only `<- ...values` from explicit mixed sources
  such as `<- (fallback, ...values)`.
- Flecha now accepts a versioned package graph whose dependency aliases are
  resolved relative to each importing package, preserving source and nominal
  identity across recursive graphs while keeping compatibility with `--lib`.
- Longbow now resolves recursive path, exact-revision Git, and owner-scoped
  registry dependencies; writes deterministic locks; verifies a shared
  content-addressed cache; and supports locked offline builds.
- Longbow now provides `add`, `remove`, `fetch`, `update`, `publish --dry-run`,
  `publish`, and `yank`, with deterministic source-only archives and the
  versioned public registry client protocol.

### Security

- Package archives reject traversal, symbolic links, case collisions,
  unsupported payloads, and checksum mismatches. Git dependencies are exported
  as blobs without running checkout hooks, filters, submodules, or package code,
  and floating revisions and remote-helper transports are rejected.

## 0.2.5 - 2026-10-27

Flecha 0.2.5 allows Longbow to create and operate on projects whose names are
not Flecha identifiers.

### Fixed

- Longbow project names now accept spaces and hyphens, and `check`, `build`,
  `run`, `test`, and `clean` accept an optional project directory.

## 0.2.4 - 2026-10-25

Flecha 0.2.4 corrects test selection for logical modules composed from
multiple source files.

### Fixed

- `flecha test <file>` now executes only tests declared in the requested source
  file while still loading the complete logical test module for shared
  declarations, imports, and semantic analysis.
- `longbow test` now runs every source file that declares a test module, so
  project-wide test execution retains complete coverage under file-local test
  selection.

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
