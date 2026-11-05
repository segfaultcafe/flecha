# Flecha 0.3 performance qualification

This document is the reproducible performance contract for Flecha 0.3. It
exists so a later release can compare like with like instead of relying on an
unversioned chart or a remembered result. The sources live in
`benchmarks/performance`, and `tools/performance.py` compiles, validates,
measures, rates, and reports them.

Performance results are evidence for these exact programs on the recorded host
and toolchains. They are not a general language ranking. Correctness, ownership,
and bounds checks are never disabled to improve a score.

## Versioned workload contract

Every implementation prints one unsigned 64-bit checksum. A measurement is
discarded if the process fails, times out, or prints a different checksum.
Constants and algorithms must change together in all five implementations and
in the harness metadata.

| Assignment | Work | Expected checksum | Purpose | Tier-scored |
| --- | ---: | ---: | --- | --- |
| `rounds` | 10,000,000 rounds | 4,536,021,502,390,955,919 | Ordinary rotate/XOR/wrapping-add state loop | yes |
| `collections` | reserve, push, and traverse 4,000,000 `u64` values | 7,999,998,000,000 | Common known-size dynamic-array construction | yes |
| `utf8` | validate/count 4,194,304 mixed-width code points | 4,194,304 | Full ASCII and 2/3/4-byte UTF-8 validation | yes |
| `fnv` | process 16,777,216 bytes | 6,371,271,965,115,818,789 | Generic byte traversal, XOR, and wrapping multiplication | yes |
| `collections_growth` | grow, push, and traverse 1,000,000 `u64` values | 499,999,500,000 | Default capacity growth and relocation diagnostic | no |
| `sha256` | process 524,288 bytes | 14,996 | Flecha standard-library hashing qualification | Flecha gate only |
| `buffered` | buffer 1,048,576 bytes | 1,048,576 | Flecha buffered-writer qualification | Flecha gate only |

The scored assignments represent work most native implementations encounter:
integer kernels, dynamic arrays, validated text, and byte hashing. The
growth-heavy collection case is intentionally retained outside the tier score.
It prevents a good aggregate rating from hiding allocator or collection-growth
weaknesses.

The language sources use the same loop counts and data patterns. C and Rust use
explicit wrapping unsigned arithmetic; Go's unsigned arithmetic already wraps;
Python masks arithmetic to 64 bits. UTF-8 implementations perform the same
validation rather than comparing a Flecha validator with another language's
unchecked byte count. Collection implementations use their ordinary growable
array type; the C diagnostic implements the same doubling policy explicitly.

## Compilation contract

- Flecha: native `build`, whose v0.3 pipeline is LLVM O2.
- C: the selected C compiler with `-O2 -DNDEBUG -std=c17`.
- Rust: `rustc -C opt-level=2 -C debuginfo=0`.
- Go: ordinary optimized `go build`, with path/debug metadata stripped.
- Python: the selected interpreter; compilation measures bytecode generation,
  and execution remains interpreted.

Compiler wall time and peak resident memory are recorded separately from
generated-program execution. Competitive compilation numbers are
informational because these toolchains perform different amounts of work.
Flecha compiler/runtime measurements and package-resolution measurements can
be release gates against a reviewed same-runner baseline.

## Sampling, budgets, and noise

The scheduled campaign uses warmups followed by repeated samples. Runtime
languages are measured in a deterministic rotating order on every sample so
frequency and thermal drift are not consistently assigned to one language.
The JSON preserves every raw wall/RSS sample plus minimum, P10, median, mean,
P90, median absolute deviation, and maximum. The human report and SVG are
derived from that JSON.

The default per-process timeout is 120 seconds. The scheduled workflow also has
a job-level time budget. Shared CI runners use deliberately broad regression
tolerances; close optimization decisions and release claims require an idle,
controlled host. Process start-up is included, so kernels must be long enough
that launch noise is visible in the reported P10–P90 interval rather than
silently ignored.

## Competitive tier policy

Only optimized runtime medians for the four scored assignments determine the
tier. A win means a strictly lower median; ties are not wins.

| Tier | Rule |
| --- | --- |
| S | beat C on more than 50% of assignments |
| A | beat Rust on more than 50% of assignments |
| B | beat Rust on at least one assignment |
| C | beat Go and Python on every assignment |
| D | beat Go on at least one assignment |
| F | beat Go on no assignments |

Rules are evaluated from S through F in that order. The v0.3 release floor is
C and the optimization target is B. Because B is evaluated before C, a B result
proves at least one Rust win but does not imply every assignment beats Go; the
per-assignment table and non-scored diagnostics must always accompany the tier.

## Reproduction

Build Flecha and Longbow, run a controlled campaign, render reports, and enforce
the v0.3 tier floor:

```sh
cmake -S . -B build-performance -G Ninja \
  -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-performance --target flecha longbow --parallel
python3 tools/performance.py run \
  --flecha build-performance/flecha \
  --longbow build-performance/longbow \
  --std std/src \
  --build-dir build/performance/work \
  --samples 11 --warmups 3 --timeout 120 \
  --output build/performance/results.json \
  --markdown build/performance/results.md \
  --svg build/performance/results.svg
python3 tools/performance.py competitive-check \
  --results build/performance/results.json --minimum C
```

The quick, deterministic harness check does not run noisy timing gates:

```sh
python3 tools/performance.py self-test
```

## Baseline lifecycle

Baseline JSON is runner- and toolchain-specific. Generate a candidate only from
a reviewed qualification run on the same controlled runner used by the gate:

```sh
python3 tools/performance.py baseline \
  --results build/performance/results.json \
  --output build/performance/baseline-candidate.json \
  --platform-id ubuntu-24.04-x86_64 \
  --wall-tolerance 40 --rss-tolerance 25
python3 tools/performance.py check \
  --results build/performance/results.json \
  --baseline benchmarks/performance/baselines/ubuntu-24.04-x86_64.json
```

Do not update a baseline merely because a gate failed. Review the raw samples,
host image, toolchain versions, generated LLVM/assembly when relevant, and the
source diff. Commit a new baseline only when the performance change or runner
transition is understood and accepted. Keep old release reports as workflow
artifacts (or under a versioned release-report directory) so v0.4 can compare
both the unchanged v0.3 contract and any explicitly versioned v0.4 workloads.

The first controlled v0.3 qualification result is preserved in
[`benchmarks/performance/reports/v0.3.0-arch-linux-x86_64.md`](../benchmarks/performance/reports/v0.3.0-arch-linux-x86_64.md).
It records the host, toolchains, command, medians, P10–P90 intervals, tier proof,
and non-scored diagnostics. Future releases should keep this workload contract
unchanged for longitudinal comparison, even when they add a separately
versioned workload set.

## v0.3 optimization findings

The qualification work exposed three concrete generated-code costs:

- checked `stringv` byte access crossed the runtime ABI on every access, which
  hid the fast path from LLVM; the compiler now emits the check and direct load
  while preserving the existing runtime diagnostic on failure;
- `RawArray` duplicated capacity in a field even though its counted owning
  buffer already carries the same value; it now derives capacity from that
  descriptor, removing stale state and allowing redundant bounds checks to
  collapse; and
- LLVM's default loop-unroll and inliner choices cloned Flecha cleanup and cold
  allocation/error paths into hot scalar loops. Native O2 keeps vectorization
  but disables the harmful unroller and uses a measured, smaller inline
  threshold.

Trivially destructible raw-storage elements also no longer receive
unobservable zero writes when extracted, relocated from, or immediately about
to be released. Ownership-bearing values still run typed destruction and clear
moved-from owners. These changes preserve safety and failure behavior; focused
compiler and standard-library tests cover the new invariants.

The reserved collection case remains a real optimization opportunity. On the
controlled v0.3 host Flecha was 27.0% slower than Go for reserve/push/traverse,
while Flecha was 14.3% faster in the default-growth diagnostic. Direct
`GODEBUG=gctrace=1` runs observed one Go collection in the reserved case and
seven in the growth case. This demonstrates why “Flecha has no GC” is not by
itself a performance explanation: the reserved hot path is dominated by push
semantics and generated control flow. Flecha propagates a recoverable allocation
error for every push and explicitly releases owned storage; Go's append fast
path has simpler success-path semantics and leaves process-exit reclamation to
the operating system. Optimizing this gap must not remove bounds checks or
weaken allocation-failure behavior.
