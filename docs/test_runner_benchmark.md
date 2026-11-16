# Test runner benchmark — 2026-11-16

The matched full-suite comparison improved from **212.80 seconds to 56.06
seconds**, a **3.80× speedup / 73.7% reduction**, with all 1,636 tests executed
and no cached results. This isolates bounded parallel execution and the runner
from the narrower coverage of focused development checks.

| Workload | Wall time | Selected | Executed | Reused |
| --- | ---: | ---: | ---: | ---: |
| Original `make test`, serial | 212.80 s | 1,636 | 1,636 | 0 |
| Four workers, full uncached | 56.06 s | 1,636 | 1,636 | 0 |
| Four workers, populate cache | 58.62 s | 1,636 | 1,636 | 0 |
| Four workers, warm full (median of two) | 54.90 s | 1,636 | 1,004 | 632 |
| Focused parser test-file selection, cold | 2.24 s | 119 | 119 | 0 |
| Same focused selection, warm | 1.50 s | 119 | 0 | 119 |

Every executed test passed. Warm full samples were 56.00 and 53.79 seconds.
Compared with cache population, the warm median is about 6.4% faster. The
standard-library aggregate and native integration work still execute, so the
38.6% reduction in test launches does not imply a comparable wall-time saving.
Focused cache reuse reduced elapsed time by about one third in this example.
Focused coverage is intentionally narrower and still requires the full CI gate.

## Method

Baseline source: `868427d22b2c6702ed32e69ea1f589be36701fb9`. The benchmark froze
that test cohort while measuring the runner changes. Afterwards the branch was
updated from master, including `6ee42d3` (async resumable lowering), and live
progress output was added at the user's request. Final integration validation
is recorded separately in the pull request; the table does not mix the newer
test inventory into the original comparison.

After that update, all **1,659 integration-suite tests** passed uncached, and
all **61 Python runner regression tests** passed. The updated full run took
65.47 seconds including rebuilding the newly merged async sources. A final
parser selection also passed with 119 executions, followed by 119 cache hits
and zero executions, confirming reuse with the final progress-enabled runner.

Host: Linux x86-64, 12 logical CPUs, CMake 4.4.2, GNU C/C++ 16.2.1, LLVM 22.1.8.
The build type was unspecified, matching the existing Makefile default.
These versions differ from CI's pinned compiler/LLVM toolchain; CI speedup must
be measured on its own runners.

The one-time clean preparation build took 81.45 seconds with three compilation
workers. It is excluded from the table. Both compared validation paths had
prepared builds, and their wall times include configuration where applicable,
incremental build checks, selection, cache checks, and execution. The uncached
runner's test phase alone took 55.25 seconds; original CTest took 212.44 seconds.

Full cold/warm runs used identical `make test TEST_JOBS=4` invocations and
report arguments, so Make's inherited environment remained stable. The cache
file was removed before population. Focused runs used the direct Python runner
with `--changed src/tests/test_parser.cpp`; the cold run added `--refresh`.
No parser source changes were made for this controlled selection experiment.

Except for the two warm full samples, these are single local observations, not
a statistical performance guarantee. No sanitizer campaign, release packaging,
or remote CI timing is included.

## Output and reproduction

The original console log had 3,328 lines. The first compact runner had five,
while retaining complete logs on disk. The final runner additionally prints
live progress: terminal bars or a short line with dots/counts every five seconds.
Use `--quiet-progress` for compact machine consumption.

Machine-readable measurements and exact commands are in
[`benchmarks/testing/local-2026-11-16.json`](../benchmarks/testing/local-2026-11-16.json).
Local raw logs/reports are retained in `.cache/test-runner-benchmark/`, and test
logs/JUnit files are in `build-tests/test-results/`. Follow
[the runner guide](test_runner.md#measuring-changes) to repeat the experiment
on another machine. Preserve separate baseline and improved checkouts/builds
when repeating a comparison across revisions.
