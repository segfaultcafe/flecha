# Changelog

## Unreleased

### Language

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

- Development builds of Flecha and Longbow now include the commit distance,
  abbreviated Git revision, and dirty-worktree state in `--version` output.

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
