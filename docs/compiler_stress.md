# Deterministic compiler stress qualification

Flecha's compiler stress suite is compiler hardening, not security testing. It
generates unusual but valid or deliberately invalid local programs, compiles
them within explicit process budgets, and keeps generated sources and artifacts
under the selected build directory. The repository retains only the C++
generator, its fixed seed, assertions, and stable diagnostic expectations.

## Corpus profiles

All profiles use seed `15120261105` and the versioned metrics format
`flecha-compiler-stress-v1`.

| Profile | Use | Declarations | Generic specializations | Nesting | Modules | Packages | Array elements | Repetitions | Per-process budget |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `fast` | Every pull request through CTest | 240 | 80 | 192 | 24 | 12 | 4,096 | 3 | 20 s, 1,024 MiB |
| `sanitizer` | ASan and UBSan compiler runs | 320 | 100 | 192 | 32 | 16 | 8,192 | 2 | 60 s, unrestricted virtual address space |
| `campaign` | Weekly and manual release qualification | 4,000 | 800 | 1,024 | 220 | 128 | 65,536 | 5 | 120 s, 4,096 MiB |

CTest also applies a five-minute outer timeout to the fast suite. GitHub's
scheduled jobs have a 30-minute job timeout. A child process that crashes,
exceeds its time budget, or, in an ordinary profile, exceeds its memory budget
fails the run instead of stalling CI. The sanitizer profile cannot use a hard
virtual-address limit because AddressSanitizer reserves a multi-terabyte shadow
mapping; it still records peak resident memory and is bounded by both child and
job timeouts.

The generated corpus covers tokenizer volume, many declarations, deep valid
and invalid expressions, diagnostic floods, distinct generic structure
specializations, ownership transfer and cleanup, checked references and
slices, recursion, large fixed arrays, extreme numeric values, logical-module
chains, duplicate declarations, module and package cycles, package-local
dependency aliases, lowering, LLVM generation, object generation, native
linking, and execution. A `ModuleLoader` instance is reused across a large and
then a minimal graph to detect stale state that separate CLI processes would
not expose.

Repeated runs require byte-identical diagnostics, LLVM, native objects, and
linked executables. The report records stable FNV-1a artifact identities plus
wall time and peak child-process memory. Timing and memory measurements are
observations, not golden values; correctness and determinism assertions do not
use noisy performance thresholds.

## Commands

Run the ordinary pull-request corpus:

```sh
make test-stress
```

Run the large release-qualification campaign:

```sh
make test-stress-campaign
```

Run the compiler and stress suite with AddressSanitizer and
UndefinedBehaviorSanitizer:

```sh
make test-sanitizers
```

The scheduled/manual workflow in `.github/workflows/compiler-stress.yml` runs
both the large ordinary campaign and a sanitizer profile, then uploads their
JSON reports.

## Diagnostic bound

A malformed source can contain an arbitrary number of independent bad tokens.
`DiagnosticBag` retains the first 100 diagnostics, appends one
`FLECHA-C0006` diagnostic stating that the limit was reached, and discards the
rest. Thus output is deterministic and bounded at 101 records while preserving
substantial context. The fast and large corpora feed 1,200 and 20,000 bad
tokens respectively and verify the same output across repeated compiler
invocations.

## Cipher-shaped safety-check measurements

The microbenchmark source is generated locally and resembles the dataflow of
SIMON, PRESENT, and Ascon inner loops: small fixed arrays, constant and variable
indices, rotations, XOR, additions, and repeated rounds. It is neither a
cryptographic implementation nor a security test.

Compiler and runtime code establish the following behavior:

- Fixed-array indexing emits an unsigned `index < length` test followed by
  `llvm.trap` before any `getelementptr`. Statically out-of-range constants are
  rejected by semantic analysis. Valid constant indices still enter the
  straightforward raw IR with checks, which LLVM O2 removes.
- Checked-slice indexing emits the same unsigned test against the runtime
  descriptor length. A genuinely variable slice index retains its guard after
  O2.
- A variable fixed-array index retains its guard. A loop induction variable
  constrained by `index < values.len()` is proven safe and its redundant guard
  is removed by O2.
- Primitive integer `+`, `-`, and `*` lower to LLVM operations without `nsw`
  or `nuw`, preserving Flecha's wrapping policy.
- Checked arithmetic lowers through LLVM `*.with.overflow` intrinsics.
  Wrapping arithmetic returns the low-width result; saturating arithmetic uses
  the overflow result to select the documented type bound.
- Native object generation runs LLVM's target-aware default O2 pipeline before
  instruction selection. `--emit-llvm` remains deliberately straightforward,
  so the stress tool also runs the default O2 module pipeline and inspects both
  forms.

The suite requires O2 to remove checks from the constant-index and proven loop
functions while retaining checks in the variable fixed-array and checked-slice
functions. It does not weaken or bypass a safety check. On the initial Linux
LLVM 22 qualification run, the complete cipher-shaped module went from 43 raw
trap call sites to 2 after O2; exact counts are recorded in each generated JSON
report, while the permanent assertions are structural so supported LLVM
versions may legitimately optimize other code differently.

When a future workload retains an unnecessary check, minimize it as a focused
regression before changing optimization behavior. Safety semantics take
priority over benchmark results.
