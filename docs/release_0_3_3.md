# Flecha 0.3.3 Longbow compatibility release

Flecha 0.3.3 fixes Longbow's compatibility check for Git-derived development
toolchains. Compiler, Longbow, runtime, standard-library, and editor artifacts
remain the same coordinated toolchain release.

## Development versions

A checkout on the 0.3.3 maintenance line without a matching release tag reports
`0.3.3-dev-g<commit>`, with `-dirty` appended when tracked files differ from the
commit. Longbow evaluates this build identity as the base release `0.3.3` for
Flecha toolchain requirements. For example, it satisfies:

```toml
[package]
flecha = ">=0.3.0, <0.4.0"
```

This normalization applies only to Flecha's Git-derived toolchain identity.
Package versions retain standard prerelease matching behavior.

## Upgrade

Install the compiler and Longbow from the same 0.3.3 build. No Flecha source or
manifest changes are required for projects already compatible with the 0.3
toolchain line.
