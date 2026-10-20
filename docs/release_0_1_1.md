# Flecha 0.1.1 stabilization milestone

Flecha 0.1.1 is a focused stabilization release. It strengthens the first
public release without starting another language-feature wave.

## Scope

1. Fix compiler crashes and incorrect diagnostics.
2. Test release bundles on clean Linux, Windows, and macOS installations.
3. Improve diagnostics for missing linkers, LLVM installations, and platform
   SDK components.
4. Pin and document the supported compiler, LLVM, linker, and platform
   toolchain versions.
5. Package the Visual Studio Code extension as a `.vsix`.
6. Document straightforward Visual Studio Code and Neovim installation
   workflows.
7. Reconcile outdated v0.1 documentation with the feature set and limitations
   that actually shipped.
8. Keep all existing CI, sanitizer, packaging, installed-layout, and
   portability checks green.

The authoritative compiler, LLVM, linker, SDK, and release-runner matrix is
documented in [`toolchains.md`](toolchains.md) and enforced by CMake and both
GitHub Actions workflows.

The release workflow also builds `flecha-language-VERSION.vsix` from the locked
Node.js dependencies, validates its installation manifest and contents, checks
its version against the release tag, and publishes it with the platform bundles
and consolidated checksums. User installation and local packaging are covered
by the [`editor installation guide`](editors.md).

At the 0.1.1 release, the documentation reconciliation treated feature guides
and executable tests as the implementation contract. That revision marked
checked references, slices, allocation, generics, and LLVM/native code
generation separately from proposals such as borrowed returns, general move
semantics, cross-compilation, and debug information. Subsequent development has
implemented single-origin borrowed returns and whole-value aggregate moves; the
current feature guides describe their contracts.

## Non-goals

Flecha 0.1.1 does not include self-hosting, a package registry, a language
server, a formatter, the project website, or major syntax and language
features. Those remain post-0.1.1 work.

## Release condition

The milestone is complete when all eight scope items are satisfied from one
revision on `master`, the full protected CI matrix passes, and the v0.1.1
release workflow publishes verified bundles for every supported platform.
