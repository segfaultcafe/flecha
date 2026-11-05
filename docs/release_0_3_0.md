# Flecha 0.3.0 release and upgrade notes

Flecha 0.3.0 is the application-capable release. It combines a broader
portable standard library with deterministic package graphs, a source-package
registry protocol, stronger compiler hardening, and qualification of clean,
relocated release archives. The detailed design and acceptance record remains
in the [0.3 milestone](release_0_3.md); this page is the user-facing release
summary and upgrade guide.

## Highlights

- Longbow resolves recursive path, exact-revision Git, and owner-scoped
  registry dependencies into deterministic locks and supports locked offline
  rebuilds, publishing, and yanking.
- The standard library adds blocking process, filesystem, path, time, random,
  DNS, TCP, UDP, buffered-I/O, SHA-2, HMAC, collection, testing, fixture, and
  benchmark facilities documented in the
  [public API index](standard_library.md).
- The compiler supports package-scoped dependency aliases, inferred generic
  method arguments, forwarding packs, and compiler-derived JSON defaults.
- Release qualification exercises compiler stress, deterministic hostile-input
  campaigns, failure injection, sanitizer runs, repeated invocation, resource
  budgets, and clean installed packages on every release target.
- The versioned performance workload is B tier on both the controlled host and
  pinned Ubuntu runner: Flecha beats Rust on the `rounds` assignment and beats
  Python on all four scored assignments. The complete per-assignment results,
  including the remaining collection and FNV opportunities, are preserved in
  the [versioned performance documentation](performance.md).

## Supported release targets

The published archives are qualified on:

- Ubuntu 24.04 x86-64 with Clang/LLVM 18;
- Windows Server 2025 x86-64 with MSYS2 UCRT64 Clang/LLVM 23;
- macOS 15 x86-64 with Homebrew Clang/LLVM 18 and Xcode 16.4; and
- macOS 15 arm64 with Homebrew Clang/LLVM 18 and Xcode 16.4.

The exact source-build ranges, linker requirements, SDKs, and installation
packages are in the [toolchain matrix](toolchains.md). Native MSVC and the
`windows-msvc` ABI are not supported in 0.3.0; their qualification is tracked
as a focused [0.3.1 toolchain patch](https://github.com/segfaultcafe/flecha/issues/188).

## Upgrading from 0.2.x

Install the matching 0.3.0 compiler archive and VS Code extension together.
Flecha, Longbow, the bundled runtime, and editor package use the same release
version, and the release workflow rejects a mixed-version bundle.

Regenerate project locks with 0.3.0 before relying on offline builds. Review
and commit the resulting `longbow.lock`; dependency aliases are now scoped to
the package that declares them, and registry/Git packages are pinned and
content-verified. Existing local `--lib` workflows remain supported.

Projects using legacy `private module` declarations should migrate to ordinary
modules with explicit `quiver` declarations for namespace-visible APIs.
`private module` remains accepted for compatibility but is deprecated, and an
`export` declaration now consistently denotes public visibility.

The JSON decoration is a compiler-known 0.3.x contract. Upgrade the compiler
and `flecha/json` package together if generated JSON readers or writers are in
use. The public decoration and generated method spelling are intended to remain
stable while the implementation moves to general decoration behavior after
0.3.x.

Application code should treat allocation, process, filesystem, network, and
parsing failures as the documented typed errors. The optimized native pipeline
does not weaken bounds checks, ownership cleanup, or fallible collection
operations. Code that depended on unchecked behavior is not a supported
migration path.

## Qualification and known limitations

The release gate requires the compiler/runtime suite, standard-library suite,
sanitizers, deterministic compiler and property campaigns, failure-injection
and soak tests, package relocation/offline tests, and the four-target package
matrix to pass from the release commit. Qualification reports record their
compiler version, repetitions, and deterministic evidence and are retained as
CI artifacts.

There are no known reproducible release-blocking crashes, hangs,
memory-safety/ownership violations, or silent-corruption defects at release
preparation time. Remaining documented limitations include:

- no native MSVC toolchain or `windows-msvc` ABI support in 0.3.0;
- no async language/runtime, HTTP/TLS/WebSocket stack, or concurrency memory
  model;
- source-only public packages, without native package build scripts or a
  stable general-purpose aggregate ABI; and
- a measurable reserved dynamic-array push gap on the controlled performance
  host, retained in the versioned benchmark rather than hidden by weakening
  safety or failure semantics.

## Verifying a download

Verify an archive or VSIX against the published `SHA256SUMS`, extract it to a
fresh directory, and confirm the paired tools report the release version:

```sh
sha256sum --check SHA256SUMS --ignore-missing
flecha --version
longbow --version
```

Both version commands must print `0.3.0`. The release assets are published only
after the same archives pass clean-install, relocation, repeat-execution, and
offline package-graph smoke tests.
