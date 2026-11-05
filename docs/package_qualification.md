# Installed-package qualification

Flecha 0.3 qualifies the shipped system from release archives rather than from
the source or build tree. The gate runs on Linux x86-64, Windows x86-64, macOS
x86-64, and macOS arm64. A separate Linux run builds the compiler, Longbow, and
runtime with AddressSanitizer and UndefinedBehaviorSanitizer before packaging
and relocating that bundle.

## Contract

Each platform job builds an archive, transfers it through the CI artifact
service, extracts it into a different directory, and runs without a source
checkout. The relocated bundle must:

- report matching Flecha and Longbow versions;
- contain every non-system runtime dependency and the bundled `lib::std`;
- check, lower, emit, link, and repeatedly execute native programs;
- compile and repeatedly execute the callable ABI and TCP loopback fixtures;
- create and cleanly rebuild a Longbow project with deterministic output;
- build, test, and run the complete standard-library API tour;
- build and run the `workspace_guard` CLI and reproduce its canonical manifest;
- build, test, and run the ledger twice and reproduce its canonical bytes; and
- resolve the pinned community package into an empty cache, preserve its lock
  and graph, delete build outputs, then rebuild with registry access forced
  offline.

`cmake/release_smoke.cmake` writes `qualification-report.txt` only after every
check passes. The report records the compiler version, repetition counts, and
hashes of deterministic outputs. CI retains the report and complete CMake log
for every platform. The release workflow runs the same script against the
exact archives that become release assets.

## Commands

From an ordinary build or staged installation:

```sh
cmake \
  -DFLECHA_COMPILER=/absolute/bundle/bin/flecha \
  -DLONGBOW=/absolute/bundle/bin/longbow \
  -DOUTPUT_ROOT=/tmp/flecha-package-qualification \
  -DCOMMUNITY_PACKAGE_ROOT=/absolute/consumer-fixture \
  -DWORKSPACE_GUARD_ROOT=/absolute/examples/workspace_guard \
  -DLEDGER_ROOT=/absolute/examples/ledger \
  -DSTANDARD_LIBRARY_EXAMPLE_ROOT=/absolute/examples/standard_library \
  -P cmake/release_smoke.cmake
```

The pull-request workflow gives release-package construction 45 minutes,
relocated consumption 30 minutes per platform, and the complete sanitizer job
60 minutes. Individual TCP compilation and execution are additionally bounded
at 60 and 15 seconds. Exceeding a budget fails the gate instead of leaving a
runner hung.

The fixtures contain no random seeds. Their inputs, expected output, lockfile,
package graph, CLI tree, ledger payloads, and expected SHA-256 values are
checked in. The public-registry portion selects immutable, already-yanked
versions through the checked lock so a qualification run cannot silently move
to a newer dependency.

## Evidence lifecycle

Pull-request artifacts are named `package-qualification-<target>` and
`sanitizer-package-qualification`. Release runs retain the same per-target
reports beside the archive-building logs. A failure before the final report is
still diagnosable from the always-uploaded log; a successful report is the
machine-readable proof that the whole installed-system contract completed.
