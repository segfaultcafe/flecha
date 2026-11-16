# Faster local validation

The Python runner builds incrementally, selects relevant CTest cases, executes
them with bounded process parallelism, and reuses successful results for a
reviewed subset of deterministic unit tests. CMake/Ninja still own compilation;
CTest still owns execution, resource scheduling, timeouts, and test fixtures.

## Commands

| Command | Selection | Successful-result cache |
| --- | --- | --- |
| `make test-changed` | Changes relative to `master`, plus local edits | Enabled |
| `make test-plan` | Explain changed-test selection | No test execution |
| `make test` | All registered tests | Enabled |
| `make test-full` | All registered tests | Disabled; CI integration policy |
| `make test-runner-self-test` | Python runner regression tests | Disabled |

Use focused validation during iteration. CI executes every test without result
reuse before merge; broad compiler/runtime and unknown changes also select all
tests locally. Once the appropriate checks pass, rerun only after additional
changes or an unresolved failure. Sanitizer, editor packaging, and release
qualification targets retain their existing purposes.

```sh
make test-changed TEST_BASE=master TEST_JOBS=4
make test-changed TEST_ARGS='--regression ^ParserTests\.'
make test TEST_ARGS='--refresh'  # rerun, then replace eligible success records
make test-full TEST_JOBS=2      # lower concurrency on a memory-constrained host
```

`TEST_JOBS` defaults to four in Make; the direct Python CLI defaults to at most
four available CPUs. The runner uses the same bounded count for compilation;
`TEST_ARGS='--build-jobs 2'` can lower it independently. Existing standalone
`make build-tests` remains available. The runner requires Python 3.10+ and an
already configured test build when invoked directly, unless `--configure` is
provided. Make's execution targets configure under the runner's lock;
`make test-plan` requires an existing build. It works with CTest 3.20;
result reuse requires CTest 3.21+ for machine-readable JUnit results. CTest 3.29+
also supports selected-name files; older versions use exact anchored regexes
and reject selections exceeding a portable command-line size limit.

Reports go to `build-tests/test-results/latest.json`, with uniquely named full
logs and JUnit XML beside it. `TEST_ARGS='--report /path/result.json'` retains a
named report. Reports include selection reasons, selected/reused/executed test
names and counts, failures/skips, and build/selection/cache/test/total durations.
The log path prints before work starts. Live progress shows a completion bar in
terminals, or loading dots with completed counts every five seconds in captured
output. Configure/build stages show elapsed time; they do not invent percentage
estimates. `--quiet-progress` keeps only the summary and log paths. CI uploads
the detailed artifacts even when validation fails.

## Selection policy

The runner combines changes from the merge-base of `TEST_BASE` and `HEAD`,
staged and unstaged changes against `HEAD`, and nonignored untracked files.
Both sides of a rename participate. A missing base or failed Git inspection
selects the full suite rather than treating the tree as unchanged.

- Changed C++ test files select their declared GoogleTest suites, discovered
  from the current file and matched against CTest's inventory.
- Changed verification helpers select commands that invoke that helper.
- Analyzer/LSP and Longbow changes select their known test dependents.
- Documentation selects the documented-example smoke test. VS Code changes
  select the existing CTest editor check; npm test/compile/package remain
  separate editor requirements.
- Shared compiler/runtime code, standard-library changes, build infrastructure,
  fixtures, removed/unreadable test files, unmatched rules, and unknown paths
  select all tests. Standard-library selection is intentionally broad because
  imports cross module boundaries and `FlechaModules` aggregates logical modules.

Explicit `--regression REGEX` filters add tests to automatic selection. An
unmatched/invalid filter is an error. `--changed PATH` can be repeated to replace
Git discovery for reproducible experiments; it is not the normal validation
policy. An unchanged tree selects zero tests and says so; use `--full` for an
integration check.

## Cache correctness and limits

The cache is local to a build directory and currently enabled only on Linux,
where runtime library dependencies are resolved with `ldd`. Unsupported hosts
or incomplete dependency inspection execute tests normally without reuse.

Only explicitly reviewed, in-memory GoogleTest suites opt in through
`PURE_SUITES` in `tools/test_cache.py`. Adding tests to these suites must preserve
the contract: no external files/state, subprocesses, network, clocks, or random
inputs. Runtime/process/network tests, integration scripts, stress/soak tests,
and the standard-library aggregate always execute. CTest fixtures, dependencies,
environment overrides, and unrecognized execution properties disable reuse.

Each successful entry covers the execution inputs:

- Test executable, resolved runtime libraries/loader, CTest and configured
  compiler binaries, CMake cache, compilation commands, and generated test
  definitions. Both direct GoogleTest commands and the standard CMake launcher
  are recognized; unknown launchers execute without reuse.
- Exact CTest command and properties, platform, runner/cache implementation,
  loader configuration, process resource limits/identity, and the complete
  inherited environment.

The cache persists hashes only, never environment values. Missing or unreadable
dependencies, corruption, GoogleTest/sanitizer overrides, and loader injection fail
closed. Only actual JUnit passes are recorded; failed, skipped, disabled, or
missing cases cannot become cached successes. Forced reruns invalidate old
successes before execution, including runs interrupted before results arrive.
Input snapshots before and after execution must agree, including all-cache-hit
runs; otherwise the runner reports that validation must be repeated. This
mutation guard also hashes tracked and nonignored untracked sources, initialized
submodules, and local changes. Fresh CTest discovery must agree after execution.

The source mutation guard is separate from the reuse key: these reviewed tests
never read source files and the runner always builds before lookup, so an
unrelated documentation edit can reuse unchanged compiled tests. Changing the
shared `test_flecha` binary invalidates all its cached cases. The cache does not
claim per-function dependency analysis or reuse expensive integration outcomes
with undeclared inputs. Its main gain is avoiding repeated process launches for
unchanged, already validated unit tests. Environment changes, including Make
override variables, conservatively invalidate reuse. For repeatable cache
measurements use identical invocations, or the direct Python runner with stable
environment. Delete `build-tests/.flecha-test-cache.json` to clear it;
`--no-cache` neither reads nor writes it.

An OS lock prevents overlapping runner builds/executions/cache writes in one
build directory and releases on process exit. Direct Ninja/CTest invocations
and external source edits do not acquire this lock; avoid overlapping them.
Separate checkouts and build configurations have separate cache identities.

## Measuring changes

Measure steady-state validation separately from the first clean compilation.
Keep the revision, compiler, build type, test count, machine, and access to
loopback/process APIs consistent. Save logs and elapsed wall time for:

1. The original serial `make test`, before modifying the runner.
2. `make test-full TEST_JOBS=4`, to isolate parallel execution without reuse.
3. `make test TEST_JOBS=4 TEST_ARGS='--refresh'`, to populate the result cache.
4. The same full selection with a warm cache.
5. A representative focused selection, cold and warm, using the same explicit
   changed path for both runs.

Compare equivalent full-suite coverage separately from focused selection;
focused results intentionally execute less coverage and still require full CI.
Record cache hits alongside timings. An unchanged run that skips eligible
cases is validation reuse, not fresh execution of every test. Use several
samples for stable capacity planning; a single local before/after experiment
does not establish a universal speedup.

See [the measured local comparison](test_runner_benchmark.md) for the initial
before/after results and machine-readable measurements.
