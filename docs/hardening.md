# Deterministic property and stateful hardening

The v0.3 hardening campaign combines adversarial/property inputs with repeated
stateful and failure-injection qualification. It is reliability and release
qualification work, not security testing. All inputs are generated locally,
all random choices come from fixed seed `15215320261105`, and generated data is
kept under the selected build directory.

The implementation deliberately composes existing human-authored Flecha
standard-library fixtures. The permanent campaign code is C++ toolchain test
infrastructure; it does not generate or commit new standard-library or `.fl`
implementation.

## Profiles and budgets

The property report format is `flecha-hardening-campaign-v1`. The stateful
report format is `flecha-hardening-soak-v1`.

| Profile | Use | Source cases | Version cases | Manifests | Graphs | Archives | Max input | Runtime repeats | Stateful rounds |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `fast` | Every pull request | 500 | 1,000 | 160 | 160 | 80 | 4 KiB | 2 | 1 |
| `sanitizer` | ASan and UBSan | 2,500 | 5,000 | 800 | 800 | 300 | 16 KiB | 5 | 1 |
| `campaign` | Weekly/manual release qualification | 25,000 | 50,000 | 8,000 | 8,000 | 2,500 | 64 KiB | 20 | 3 |

CTest applies a five-minute timeout to the fast property campaign. Scheduled
jobs have a 45-minute outer timeout. Individual stateful test invocations have
a three-minute timeout, and the repeated runtime group has a fifteen-minute
timeout. These limits turn hangs into failures rather than stalled CI.

## Property campaign

The deterministic mutator performs truncation, deletion, insertion,
replacement, duplication, delimiter runs, bit flips, and arbitrary-byte
replacement. Each input is evaluated twice and must produce an identical
result. Every profile also includes inputs that reach its declared byte limit,
including repetitive token streams and bounded structured nesting.

The permanent properties are:

- token ranges remain ordered and inside the source, tokenization always ends
  at EOF, parsing always returns a compilation unit, and diagnostics never
  exceed the configured 100 records plus the limit diagnostic;
- semantic versions either reject deterministically or round-trip through
  their canonical spelling; version requirements make stable match decisions;
- manifests and package graphs accept or reject deterministically across
  malformed, truncated, oversized, escaped, duplicated, and nested input;
- archive extraction is deterministic, enforces canonical published content,
  and removes the entire destination after every rejected partial extraction;
- every campaign includes both accepted and rejected inputs, so a parser that
  accidentally accepts or rejects everything cannot pass.

If a property fails, the exact input and reason are written to the profile's
`failures` directory. A discovered defect must be minimized and committed as a
focused ordinary regression before the seed or campaign moves on.

The human-authored standard-library suite already owns canonical-form and
round-trip contracts for hexadecimal, Base64, binary integers, UTF-8, and
numeric text. The scheduled campaign reruns that complete suite after the C++
property campaign rather than duplicating those contracts in generated Flecha
source.

## Stateful and failure-injection soak

The soak runner repeatedly executes the existing runtime and Longbow C++ test
groups with GoogleTest's deterministic shuffle seed. It covers child
processes, environments, random data, secure memory operations, clocks,
filesystem mutation, file streams, package archives, registry/cache reuse, and
dependency resolution. TCP, UDP, and DNS groups join the scheduled and
sanitizer profiles.

Existing allocation-injection and lifecycle fixtures then run in alternating
forward and reverse order. They cover runtime filesystem/environment/socket
allocation, crashable formatting, assertions and benchmarks, child processes,
buffered I/O, paths, callable allocation, and every owning collection's
initial allocation, growth, lifecycle, nested ownership, algorithms, and
borrowed views. Package lifecycle and the CLI/ledger north-star fixtures are
also repeated.

Every child receives one isolated temporary directory. A run fails if any
file or directory remains there afterward. ASan/LSan and UBSan scheduled runs
provide the leak, double-cleanup, use-after-free, and undefined-behavior gate.
An exclusive build-directory lock rejects concurrent soak invocations before
they can interfere with CTest fixtures that use fixed work directories.

## Commands

Run the pull-request profile:

```sh
make test-hardening
```

Run the release-qualification property and soak profiles:

```sh
make test-hardening-campaign
```

Run compiler, property, and selected stateful campaigns with sanitizers:

```sh
make test-sanitizers
```

The scheduled/manual workflow is `.github/workflows/hardening.yml`. It uploads
the JSON reports, failure inputs, and complete soak log even when a campaign
fails.
