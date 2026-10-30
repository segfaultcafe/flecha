# Flecha 0.3.0 application and library ecosystem milestone

Flecha 0.3.0 is the application-capable standard-library and package-ecosystem
milestone. Its goal is to make substantial systems programs and independently
distributed libraries practical without turning the release into another
broad language-feature wave. The milestone expands the standard library from
its initial arrays, algorithms, files, and testing support into a coherent
foundation for command-line programs, structured data, persistent services,
networking, and deterministic ledger applications.

This is one concrete step in Flecha's broader
[direction toward 1.0 and beyond](vision.md). That direction is intentionally
adaptable: it explains what the project is trying to make possible without
freezing every future feature or assigning it to a release in advance. The
requirements in this document apply specifically to the 0.3.0 milestone.

The north-star programs for this release are:

- a nontrivial command-line application;
- a persistent, deterministic blockchain or append-only ledger prototype;
- a blocking TCP client and server;
- a package-development fixture created, published, resolved, and consumed
  through the public Flecha package registry; and
- a concurrent worker application if the required memory-model rules can be
  specified without delaying the rest of the milestone.

Passing isolated unit tests is not enough. Each north-star program must build
and run from an installed release package on every supported platform.

## Scope and principles

The standard library should be implemented in Flecha wherever the language can
express the behavior safely and predictably. Native runtime code is reserved
for operating-system boundaries, compiler intrinsics, and facilities that
cannot be implemented portably in Flecha. Public wrappers around those
boundaries must remain safe where their contracts can be enforced.

The milestone follows these rules:

- public APIs follow [`library_api_style.md`](library_api_style.md);
- Flecha sources follow [`std_development.md`](std_development.md);
- unsafe code remains behind narrow, documented safe interfaces;
- serialization is field-based and canonical rather than based on in-memory
  aggregate layout;
- allocation, copying, blocking, and platform-dependent behavior stay visible;
- deterministic APIs behave identically on every supported target;
- Longbow owns dependency acquisition and the compiler remains network-free;
- cryptographic implementations use published test vectors and established
  implementations rather than novel algorithms; and
- higher-level protocols enter the standard library only after their lower
  layers have stable contracts.

## Planned module map

The initial public module layout is:

```text
std::fmt                 typed console and owning-string formatting
std::math::*              numeric, integer, bits, floating_point, constants
std::memory::*            slices
std::bytes::*             buffer, sequence
std::containers::collections
                          algorithms, array, deque, stack, queue, heap,
                          hash_map, hash_set
std::containers::raw_array
                          low-level compatibility array
std::containers::errors   collection errors
std::text::*              ascii, utf8, builder, parse, format
std::encoding::*          binary, hex, base64
std::io::*                errors, file, buffered, stdio, filesystem, path,
                          directory, temp
std::process::*           args, environment, command
std::time::*              duration, clock
std::random::*            generator, secure
std::net::*               errors, address, dns, tcp, udp
std::hash::*              checksums, sha2, hmac
std::crypto::*            constant_time, secure_memory
std::tests::*             testing, asserts, fixtures, golden, benchmark
```

This map defines public ownership and naming rather than requiring every
operation to live in a separate source file. Closely related operations belong
in one cohesive module; submodules are reserved for independently useful API
families. Safe containers and their algorithms share the cohesive
`std::containers::collections` module; low-level raw storage remains separate.
Moving an existing public declaration requires compatibility review.

## Implementation sequence

The stages below are dependency ordered. A later stage may be developed in
parallel, but it cannot be considered complete until its required foundations
are complete.

### 1. Numeric, memory, and byte foundations

Add foundational modules for:

- minimum, maximum, clamping, and absolute-value operations;
- checked, wrapping, and saturating integer arithmetic;
- bit rotation, population count, leading and trailing zero counts, and byte
  swapping;
- safe slice copying, overlap-aware copying, filling, and swapping;
- deterministic byte-sequence equality and lexicographic comparison;
- explicit little-endian and big-endian integer encoding; and
- bounded byte readers and dynamically growing byte writers.

Every arithmetic operation must document overflow behavior. Byte encoders must
produce canonical output independent of host endianness and structure layout.
Readers must report malformed or truncated input without reading outside their
source slice.

#### Stage 1 contract and test gate

Implementation begins only after the slice contracts in
[`memory.md`](memory.md) and the arithmetic contracts in
[`math.md`](math.md) are reflected by this test matrix:

| Area | Required cases |
| --- | --- |
| Empty slices | `copy_elements` with either side empty returns zero; `copy_within(values, 0, 0, 0)` succeeds and an empty range at `len` succeeds; `fill` is a no-op; and `swap` returns `IndexOutOfBounds` without mutation. |
| Truncated copying | Source shorter than destination, destination shorter than source, equal lengths, returned count, copied prefix, and untouched destination suffix. |
| Overlap | Leftward overlap, rightward overlap, identical source and destination ranges, and nonoverlapping ranges, with results matching a preserved original source. |
| Ranges | `sourceStart > sourceEnd`, each source endpoint beyond `len`, destination start beyond `len`, destination end beyond `len`, valid empty ranges, and proof that failed validation performs no mutation. |
| Swap | First/last exchange, equal valid indices as a no-op, either index invalid, both indices invalid, and no mutation on every error path. |
| Element lifecycle | Scalars, copyable structures, and copyable structures with nontrivial owned-field copies and cleanup; verify independent copies and exactly-once cleanup of replaced values. |
| Unsupported elements | Compile-time rejection of representative move-only and immovable element types in every copy-based slice operation, using `FLECHA-S0046` for prohibited copies and `FLECHA-S0059` for move reuse. |
| Wrapping arithmetic | `wrapping_add`, `wrapping_sub`, and `wrapping_mul` for `i8`, `i16`, `i32`, `i64`, `isize`, `u8`, `u16`, `u32`, `u64`, and `usize`; cover exact results, zero/one identities, both overflow directions where applicable, signed minimum/maximum transitions, unsigned zero/maximum transitions, multiplication overflow, and target-width behavior. |

Existing byte-sequence, endian, reader, and buffer tests remain regression
requirements. Slice tests must distinguish typed semantic copies from raw byte
operations, and negative tests must compile as independent fixtures so an
expected type error cannot mask the positive runtime cases.

### 2. General-purpose collections

Complete the existing array foundation and add:

- `Array<T>` support for movable and destructible elements;
- nested owning collections;
- deque, stack, queue, and binary-heap structures;
- hash-map and hash-set structures;
- sorting, searching, partitioning, and deduplication algorithms; and
- checked borrowed views over collection storage where ownership rules permit
  them.

Until the language has a general constraint or trait system, generic
collections that need user-defined behavior accept explicit callable policies.
For example, hash maps accept hash and equality callables, and heaps accept a
comparison callable. The standard library must not add a language feature only
to make these initial APIs resemble another language's collection APIs.

Closed compile-time type sets are available for identity restrictions such as
`T: integer`. They deliberately do not replace callable policies or introduce
behavioral traits; see [type sets](type_sets.md).

Growth failure must leave a collection valid and preserve its previous
contents. Moving, removing, replacing, and destroying elements must execute
each element's cleanup exactly once.

#### Stage 2 array-ownership contract and test gate

The first Stage 2 deliverable is the ownership-capable array foundation defined
in [`collections.md`](collections.md). Later containers must reuse its storage,
relocation, failure, cleanup, and borrowing rules rather than introduce parallel
ownership machinery.

Implementation begins with an internal initialized-prefix storage facility and
the following acceptance matrix:

| Area | Required cases |
| --- | --- |
| Copy versus move | Copyable and move-only arguments for `push` and `set`; ownership-transferring `pop`, `remove`, and `swapRemove`; compile-time rejection of copy-only APIs for move-only elements. |
| Initialized storage | Only `[0, len)` is live and exposed; spare capacity is never read or destroyed; relocation makes each source slot non-live. |
| Strong failure guarantee | Initial and later allocation failure plus capacity overflow preserve length, capacity, order, and all preexisting elements. |
| Cleanup | Replacement, ordered and unordered removal, clear, growth, array destruction, and failed operations execute every applicable cleanup exactly once. |
| Nested ownership | `Array<Array<T>>`, owning slices, outer and inner growth, extraction, three-level nesting, and recursive destruction without cloning inner allocations. |
| Borrowed views | Shared and exclusive view conflicts with mutation, structural changes, reallocation, movement, replacement, and destruction; access resumes after the view's last use. |
| Unsupported elements | Compile-time rejection of unsafe-tear immovable types, borrowed element storage, partial indexed moves, and use after a moved argument or extracted owner. |

The complete Stage 2 array definition of done and test cases live in
[`collections.md`](collections.md). Later containers reuse this ownership,
storage, failure, and lifecycle foundation instead of adding parallel unsafe
machinery.

The array gate now passes: direct owning strings and slices, outer/inner and
three-level nested growth, recursive destruction, failure injection, partial
move diagnostics, and the complete shared/exclusive view matrix are covered by
native, compile-fail, full-suite, and sanitizer tests. The `Deque<T>` gate also
passes with circular move-only storage, bidirectional growth-failure coverage,
nested ownership, compile-fail diagnostics, and sanitizer fixtures. The
`Stack<T>` and `Queue<T>` adapter gates now pass the same allocation-failure,
move-only lifecycle, nested ownership, diagnostics, and sanitizer requirements.
The `BinaryHeap<T>` gate also passes with caller-supplied comparison policies,
move-only sifting, strong growth-failure preservation, nested ownership,
compile-fail diagnostics, and sanitizer fixtures. The `HashMap<K, V>` gate
passes as well, including move-only keys and values, borrowed iteration,
allocation-failure preservation, nested ownership, diagnostics, and dedicated
sanitizer fixtures. The `HashSet<T>` gate now passes through the same probing
engine with an empty marker type, including deduplication, zero-sized marker
allocation failure, move-only lifecycle, nested ownership, diagnostics, and
dedicated sanitizer fixtures. The collection-algorithm gate now passes as well:
ordered binary search, unstable partitioning, and adjacent deduplication join
the existing linear search and heap sort surface. Slice transformations remain
explicitly copy-based, while Array transformations relocate move-only values
and destroy removed duplicates exactly once.

### 3. Text, parsing, and encoding

Add:

- a mutable `StringBuilder` with explicit allocation behavior;
- integer and floating-point parsing and formatting;
- ASCII classification and case conversion;
- UTF-8 validation, decoding, and code-point iteration;
- hexadecimal and Base64 codecs;
- a canonical binary encoding layer.

The mutable builder, integer and floating-point parsing and formatting, ASCII,
UTF-8, hexadecimal, Base64, and canonical binary foundations are implemented
with focused standard-library and native-runtime tests. Base64 uses the
standard padded RFC 4648 alphabet, allocates results for callers, and strictly
rejects malformed or non-canonical input. JSON is an independently versioned
package planned for the 0.4 library wave rather than a bundled standard-library
module.

The existing `string` and `stringv` types remain byte-oriented. Unicode APIs
must not silently change their indexing or length semantics. CSV, TOML, YAML,
compression formats, schema frameworks, and general reflection remain
companion-library candidates rather than requirements for this milestone.

### 4. Package graph and community library distribution

Deliver the package model defined in
[`package_ecosystem.md`](package_ecosystem.md):

- package-scoped dependency aliases in the compiler rather than one global
  `--lib` namespace;
- recursive local, Git, and public-registry dependency resolution in Longbow;
- a deterministic `longbow.lock` containing exact sources, versions, and
  checksums;
- a shared local source cache with verified offline reuse;
- scoped registry identities such as `flecha/json` and
  `alexbsec/blockchain`, while source imports continue to use local aliases
  such as `lib::json` and `lib::blockchain`;
- immutable source archives, ownership, publishing, and yanking through the
  public Flecha registry; and
- `longbow add`, `fetch`, `update`, `publish --dry-run`, `publish`, and `yank`
  workflows.

The bundled standard library remains a toolchain component available as
`lib::std`; it does not participate in ordinary registry resolution. Public
registry packages are source-only in 0.3.0. Precompiled Flecha objects, native
artifacts, build scripts, and registry-supplied `std` replacements are outside
the initial package contract.

#### Stage 4 package-distribution contract and test gate

Stage 4 is complete only when:

| Area | Required cases |
| --- | --- |
| Package graph | Each package resolves `lib::<alias>` through its own dependency map; repeated aliases in different packages can identify different dependencies without creating one global alias namespace. |
| Resolution | Recursive path, pinned Git, and registry dependencies resolve deterministically; 0.3 selects one version per canonical owner/package identity, and diagnoses dependency cycles, incompatible version requirements, missing packages, and incompatible Flecha versions before compilation. |
| Locking | A clean resolution writes exact package identities, versions, sources, revisions, and SHA-256 checksums; subsequent builds honor the lock and reject changed content. |
| Cache | A first online build populates the cache; a second offline build succeeds from verified cached sources; corrupt or incomplete cache entries are rejected and reacquired when online. |
| Registry identity | Official and community packages share one registry under distinct owner namespaces; dependency aliases control imports and canonical owner/package identities never leak into Flecha module paths. |
| Publishing | `publish --dry-run` shows the normalized source archive and checksum without uploading; published versions are immutable; yanking prevents new selection without breaking an existing lock. |
| Standard library | Bundled `lib::std` is selected from the installed toolchain, remains path-overridable for toolchain development, and cannot be replaced by a public-registry package. |
| Portability and safety | Archives reject traversal, absolute paths, symlinks, case-colliding paths, unsupported native inputs, and platform-specific generated output. The same locked graph builds on every supported host. |

### 5. I/O and operating-system services

Expand the current file and filesystem modules with:

- buffered readers and writers;
- standard input, standard output, and standard error handles;
- path joining, normalization, filename, parent, and extension operations;
- directory iteration, recursive directory creation, and recursive copying;
- temporary files and directories;
- command-line argument access;
- environment-variable access;
- monotonic and wall-clock time, durations, and sleeping;
- operating-system-backed secure randomness; and
- process creation, exit status, and pipe support.

Resources use owning structures with deterministic teardown, following the
existing `File` model. Blocking operations must be identified in their API
documentation. Platform errors map to stable Flecha error families without
discarding partial byte counts or child-process status.

The buffered-I/O portion of Stage 5 is implemented in Flecha over the existing
stream adapters. Buffered readers preserve read-ahead bytes and defer a partial
read error until its final buffered byte. Buffered writers require explicit
flush, preserve unwritten suffixes for retry, and never perform fallible work
during destruction. Both owners move their stream adapter, own exactly one byte
allocation, propagate file borrows, and release every moved value exactly once
on success or allocation failure. Focused tests cover empty operations, direct
and buffered paths, boundary crossings, short operations, EOF, zero progress,
error retry, allocation failure, movement, borrow conflicts, file round trips,
and byte ordering without native buffering support.

Directory iteration is implemented through the owning `std::io::file::Directory`
and `DirectoryEntry` values. Entries own their names, symbolic-link
classification does not follow links, host `.` and `..` records are filtered,
EOF is stable, and deterministic teardown closes each iterator exactly once.
Allocation failure preserves the pending entry for retry. Native and Flecha
tests cover empty and populated directories, file and directory types, symbolic
links, invalid and missing paths, regular-file rejection, movement, explicit
close, repeated EOF, and allocation failure. Recursive directory creation and
copying remain separate Stage 5 work.

The standard-stream portion is also implemented. The `stderr` and `stderrln`
built-ins mirror the existing printable `stdout` operations, while
`std::io::stdio` exposes process-owned streams as non-owning fallible byte
sources and destinations. `StandardInput` supplies a captureless
`io::stream::Reader`; standard output and error supply captureless
`io::stream::Writer` adapters. These views never close process resources and
compose directly with buffered I/O.
The root `std::fmt` facade provides `print`, `println`, `printf`, and matching
standard-error operations through compile-time forwarding packs. Literal `{}`
formats are checked by the compiler, and `sprintf` returns an owning string
with explicit allocation failure. No runtime variadic ABI is introduced.
The `stdin` and `stdinln` built-ins provide typed line input into an
already-declared mutable destination. The compiler selects complete-line
conversion from the referenced type, preserves the destination on failure, and
reports EOF, read, parse, range, prompt-flush, and allocation failures
explicitly. Formatted input is intentionally absent; formatted prompts compose
through `std::fmt` followed by `stdin`.

The command-line argument portion is implemented through
`std::process::arguments`. `capture()` returns an owning runtime-sized array of
independent owning strings, including the host-supplied invocation name at
index zero. Repeated captures are independent. Array allocation and individual
argument capture failures return `AllocationError.OutOfMemory`; partial
construction releases every completed string and the array exactly once.
Compiler-generated entry wrappers retain parameterless Flecha `main`
signatures while recording the host argument table before invoking the source
entry point. Environment variables and broader process control remain separate
Stage 5 tasks.

The lexical-path portion is implemented in `std::io::path`. Joining and
normalization allocate owned results and expose allocation failure; filename,
parent, and extension queries return checked borrowed views into their source.
POSIX and Windows grammars are exercised on every host, including drive and
UNC roots, mixed Windows separators, dot components, relative leading parents,
and attempts to traverse above an absolute root. The runtime contributes only
the host path-style query; all parsing and transformation remain in Flecha and
perform no filesystem access.

### 6. Blocking networking

Add an initial `net` layer containing:

- IPv4 and IPv6 address values;
- socket addresses and explicit port handling;
- hostname resolution;
- owning TCP listener and stream types;
- owning UDP socket types;
- partial read and write operations;
- connection, read, and write timeouts; and
- safe close and teardown behavior.

The first networking APIs are blocking. Async I/O, HTTP, WebSockets, TLS, and
service frameworks are not release gates for 0.3.0. They should begin as
companion libraries after the socket and buffered-I/O contracts are proven.

### 7. Hashing and security primitives

Provide the primitives required for deterministic integrity checks and the
ledger north-star program:

- SHA-256 and SHA-512;
- HMAC over supported secure hashes;
- constant-time byte comparison;
- operating-system cryptographic randomness; and
- secure clearing where a platform can provide the promised behavior.

Cryptographic code must use published conformance vectors and receive focused
review. Ed25519, secp256k1, key-store formats, wallet formats, and TLS should
incubate as companion libraries before any later standard-library proposal.

Noncryptographic checksums and hashes should be clearly separated from the
security modules so callers cannot mistake them for cryptographic primitives.

### 8. Testing and performance support

Extend the testing library with:

- equality and ordering assertions with useful failure output;
- explicit error-family and error-variant assertions;
- byte and string comparison diagnostics;
- temporary-directory fixtures;
- golden-file helpers;
- deterministic random test data; and
- a small benchmark runner with warmup and repeated samples.

Each standard-library module must include ordinary success, boundary, failure,
and cleanup tests. Algorithms and codecs need portable reference vectors.
Containers and resource-owning types need allocation-failure and sanitizer
coverage.

### 9. Hardening, stress, performance, and stability

After the feature stages and their required Stage 8 support are complete,
freeze the public 0.3 surface and harden the complete toolchain as one shipped
system. This stage adds no planned feature area. Any API or language change
discovered here requires an explicit defect or release-blocker justification
and must restart the affected compatibility and stress gates.

Hardening includes:

- large deterministic compiler stress corpora covering parsing, semantic
  analysis, ownership, borrowing, generics, module graphs, lowering, and native
  code generation;
- adversarial, randomized, and property-based tests for parsers, codecs,
  collections, I/O state machines, package metadata, and runtime ABI
  boundaries;
- repeated allocation-failure, partial-operation, cleanup, and operating-system
  failure injection across every owning or stateful API;
- sanitizer, fuzz, and malformed-input campaigns that preserve every reduced
  reproducer as a permanent regression test;
- long-running stability and soak tests for repeated compilation, execution,
  package resolution, cache reuse, clean rebuilds, and resource teardown;
- performance benchmarks for compiler latency and peak memory, generated-code
  execution, core collections, text and encoding, buffered I/O, hashing, and
  package resolution;
- reviewed performance baselines with explicit regression tolerances rather
  than untracked benchmark numbers; and
- repeated clean-install and relocated-package tests on every supported release
  target, including deterministic output and offline dependency reuse.

#### Stage 9 hardening gate

| Area | Required evidence |
| --- | --- |
| Compiler stress | Large valid programs compile deterministically; deeply nested and invalid programs produce bounded diagnostics without crashes, hangs, stale state, or nondeterministic output. |
| Runtime stress | Long-running and high-volume ownership, allocation, I/O, networking, and process workloads complete without leaks, double cleanup, corruption, deadlock, or descriptor exhaustion. |
| Adversarial input | Tokenizers, parsers, manifest readers, decoders, protocol readers, and runtime boundaries reject malformed, truncated, oversized, and randomized input safely. |
| Failure injection | Every allocation and fallible operating-system boundary is exercised at each meaningful failure point, preserving documented state and exactly-once cleanup. |
| Sanitizers and fuzzing | Supported sanitizer suites pass; fuzz campaigns meet their recorded duration or execution budget; every discovered crash receives a minimized permanent regression. |
| Performance | Versioned benchmark workloads record compile time, peak memory, execution time, throughput, and allocation behavior where relevant; regressions beyond the reviewed tolerance block release unless explicitly accepted. |
| Stability | Repeated and reordered test runs, clean builds, incremental builds, offline builds, relocated packages, and soak runs remain deterministic and do not accumulate process or filesystem state. |
| Defect closure | There are no known reproducible crashes, hangs, memory-safety violations, ownership violations, silent data corruption, or release-blocking correctness defects. Every fixed high-impact defect has a focused regression test. |

The intent is a bulletproof regression boundary for the supported 0.3 surface:
ordinary correctness tests prove individual contracts, while this stage attacks
their composition, scale, repetition, failure behavior, and performance.

## Milestone discoveries

Implementation work may reveal useful behavior that follows from existing
language rules even when no individual feature was designed for that exact
use. Flecha 0.3.0 treats the discoveries recorded here as supported behavior,
not accidental compiler permissiveness.

### Owning captured checked references

A mutable checked reference is itself a move-only value. An `own` closure
capture may therefore move the reference handle into the closure environment
without moving or taking ownership of its referent:

```flecha
stream::Writer File::writer(&self) <-self {
    return stream::Writer::init(
        [own self](source) => {
            return self->write(source);
        }
    );
}
```

Here `self` has type `&File`. The closure owns that reference value, while the
caller continues to own the `File` and its native handle. Moving the resulting
writer moves the captured reference. Destroying it releases the borrow but
does not flush, close, move, or destroy the file.

The checked-reference origin must propagate through the `own` capture, the
callable value, enclosing structures, moves, and the `<-self` returned-borrow
contract. Safe code cannot access, move, close, or create an overlapping
mutable adapter for the file while the captured reference may still be used.
The file becomes available again after the adapter's final possible use.

This behavior requires regression coverage for successful adapter moves and
post-borrow access, plus compile-time rejection of conflicting file access,
adapters escaping a local file, invalid returned origins, overlapping mutable
adapters, and relocation of an owner while an adapter remains live. `own` in a
capture describes transfer of the captured value; when that value is a
reference, it never implies ownership of the referent.

## Minimal compiler and runtime enablement

The milestone should otherwise avoid new surface syntax. The anticipated
enabling work is limited to capabilities the library cannot safely implement
itself:

1. Specify unsigned overflow behavior or provide explicit checked, wrapping,
   and saturating intrinsics.
2. Runtime-array storage supports sized, safely movable, semantically zeroable
   elements, including destructible and nested owning values, with per-element
   reverse destruction.
3. Provide safe construction of owning strings from validated byte buffers.
4. Expose arguments, environment, time, randomness, processes, directories,
   and sockets through additive versioned runtime operations.
5. Define the minimum concurrency and memory-model rules only if concurrency
   remains in the 0.3.0 release gate.
6. Improve allocation-failure injection and lifecycle diagnostics needed to
   verify the new containers.
7. Accept a package graph whose dependency aliases are scoped to the importing
   source identity, while keeping all network and registry operations in
   Longbow.
8. Provide allocation-free compile-time forwarding packs for the small Flecha
   wrappers over typed standard-output and standard-error operations, without
   exposing a native variadic ABI or a runtime pack value.

This work should be delivered as focused compiler or runtime changes in support
of a concrete standard-library API. It must not become an unrelated language
feature wave. Any new syntax or source-incompatible behavior requires an
explicit scope decision and the normal versioning review.

## North-star ledger

The ledger example validates several stages together. It must:

- define a canonical block-header encoding without hashing structure memory;
- link blocks through fixed-size SHA-256 digests;
- append and validate blocks;
- persist and reload the chain through standard I/O APIs;
- reject malformed, truncated, and tampered storage;
- reproduce identical hashes and serialized bytes on every supported target;
  and
- use no native application-specific shim once the required standard-library
  stages are complete.

Peer-to-peer consensus, economic incentives, a smart-contract virtual machine,
and production key management are outside this example's scope.

## Release gates

Flecha 0.3.0 is ready when:

1. Stages 1 through 8 are implemented, documented, and tested, and the complete
   Stage 9 hardening gate passes.
2. The CLI, ledger, and blocking-network north-star programs
   build and run from clean, relocated release packages.
3. The complete suite passes on Linux x86-64, Windows x86-64, macOS x86-64,
   and macOS arm64 where those release runners remain supported.
4. Compiler, runtime, and native examples pass AddressSanitizer and
   UndefinedBehaviorSanitizer checks on supported sanitizer hosts.
5. Resource-owning APIs pass injected allocation and operating-system failure
   tests without leaks, double cleanup, or stale-handle use.
6. Serialization, hashing, text, and numeric operations pass portable golden
   vectors and produce deterministic cross-platform results.
7. Every exported module has reference documentation and at least one focused
   example or north-star use.
8. Packaged Longbow discovers the bundled standard library and all required
   runtime symbols without referring to the source checkout.
9. A community-package fixture can be published, resolved into a locked graph,
   built from a clean cache, rebuilt offline, yanked without invalidating the
   existing lock, and consumed under a caller-selected `lib` alias.

Concurrency is a stretch gate until its memory-model design is accepted. Its
absence must not block the remaining application-capable standard library.

## Explicit non-goals

Flecha 0.3.0 does not require:

- a trait, interface, or protocol system;
- variadic generics or partial explicit/inferred type-argument lists;
- exceptions, garbage collection, reflection, or async language syntax;
- private registries, private packages, registry federation, or registry
  mirrors;
- precompiled Flecha packages, native package artifacts, or package build
  scripts;
- HTTP, TLS, WebSockets, or an async executor in the standard library;
- novel cryptographic algorithms;
- a production public-blockchain consensus system; or
- a stable general-purpose Flecha aggregate ABI.
