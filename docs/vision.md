# Flecha toward 1.0 and beyond

Flecha is being built to become a practical, native systems language that can
eventually carry its own implementation. The work before 1.0 develops the
language, compiler, runtime, tooling, standard library, and broader library
ecosystem together. Each layer should make the next class of programs possible
without hiding allocation, ownership, blocking, or native boundaries.

This document describes a direction, not a frozen checklist. Flecha is still
young enough to learn from its own implementation. Features may be added,
replaced, reordered, or removed as real programs expose better designs. A
future capability mentioned here is not automatically promised for a specific
release, and the absence of a capability from this document does not exclude
it. Individual release milestones define their own concrete gates.

## What the foundational work is for

The early standard-library layers are deliberately composable:

```text
ownership and memory
        |
bytes, strings, containers, and allocation
        |
encoding, streams, files, and buffering
        |
   +----+----------------+----------------+
   |                     |                |
data formats          networking     compiler infrastructure
JSON and XML          sockets        source loading
Base64                HTTP           diagnostics
serialization         WebSocket      caches and artifacts
                      gRPC
```

Buffered I/O, for example, is not only a convenience for files. It becomes a
shared incremental-input layer for parsers, network protocols, compiler source
loading, and future asynchronous adapters. The goal is to build a small number
of trustworthy foundations that support many independently useful libraries.

## Directions before 1.0

The work toward 1.0 is expected to open several related directions. They can
advance in parallel, and their exact release boundaries are intentionally not
fixed here.

### Data and interchange

Flecha should be able to implement efficient, streaming codecs and parsers for
formats such as Base64, JSON, and XML. That requires mature byte and text APIs,
buffered I/O, Unicode support, numeric conversion, builders, collections, and
structured errors. Some formats may belong in the standard library and others
may remain companion packages; either result is valid if the language makes
them safe, natural, and efficient to implement.

### Security and cryptography

Programs need secure randomness, hashing, authentication, encryption, TLS,
constant-time operations, and careful key handling. Flecha should expose the
low-level control required to integrate established cryptographic
implementations and to build safe higher-level APIs around them. The project
will prefer reviewed algorithms, published vectors, and proven native
providers over inventing new cryptography.

### Networking and asynchronous I/O

Blocking sockets, DNS, timeouts, and stream adapters can support useful TCP and
UDP programs before the language has async execution. Basic HTTP and WebSocket
implementations can grow on those foundations. Async I/O can then add scalable
transport without forcing protocol parsers and framing code to be rewritten.

Later networking work may include HTTP/2 and gRPC. Those depend on additional
pieces such as Protocol Buffers, TLS, streaming, cancellation, deadlines,
code generation, and a capable asynchronous runtime. Their presence in this
direction is an ambition, not a commitment to place every layer in `std` or to
ship it by a predetermined version.

### Native systems integration

Flecha should be able to bind directly to demanding native APIs such as Vulkan
and ImGui, and eventually support native GUI libraries of its own. Doing that
well exercises C ABI compatibility, explicit layouts and alignment, raw
pointers, unions, callbacks, dynamic and static linking, build integration,
and ownership across foreign boundaries.

Vulkan, graphics libraries, and GUI toolkits are not necessarily
standard-library modules. They are proof that Flecha can express low-level
systems integration without requiring a C++ wrapper to make the hard parts
possible.

### Metaprogramming and library expressiveness

Large native APIs and schema-driven protocols need more than handwritten
boilerplate. Flecha is expected to explore macros and other compile-time
facilities for hygienic code generation, serialization, derived operations,
FFI bindings, and protocol-generated types. The exact design is deliberately
open: declarative macros, compile-time functions, reflection, derives, or a
combination should be judged by the programs they enable and the clarity of
their generated code.

### Compiler-enabling libraries

A self-hosted compiler needs the same foundations as other substantial native
applications: files and paths, buffered source loading, strings and Unicode,
maps and sets, arenas and allocation strategies, processes, diagnostics,
serialization, testing, benchmarking, and concurrency. Building those pieces
for general use also prepares Flecha to implement its own toolchain.

## What Flecha 1.0 means

Flecha reaches 1.0 when it no longer depends on another implementation
language for its ordinary continued development.

The 1.0 compiler will be written in Flecha, compile the Flecha language and its
language libraries, and be able to compile itself. The current v0 compiler,
written in C++, will remain as the legacy bootstrap implementation:

```text
legacy v0 compiler
        |
        v
stage-1 compiler written in Flecha
        |
        v
stage-2 compiler built by Flecha itself
```

Stage 1 must build stage 2 successfully, and the resulting compilers must be
reproducible or demonstrably equivalent. The complete language and library
test suites must pass through the self-hosted toolchain. Once that path is
trusted, the v0 compiler should no longer be required for ordinary development
or releases; it remains available for bootstrap and historical compatibility.

Self-hosting alone is not enough. The self-hosted compiler and the programs it
generates should be in the same performance class as equivalent native C++
implementations. This is a measurable engineering target rather than a claim
that every possible program will have identical performance. Benchmarks should
cover at least:

- generated-program execution time and memory use;
- compiler execution time and peak memory;
- compilation scalability and incremental workflows where supported;
- generated binary size; and
- the runtime cost of ownership and high-level abstractions.

Flecha 1.0 is therefore not simply the release in which the syntax is declared
finished. It is the point where Flecha can carry its own weight: implemented
in itself, able to build real native systems, supported by the libraries its
toolchain needs, and performant enough that rewriting it in C++ would provide
no fundamental advantage.

This is a north star, not a prohibition on discovery. Reaching it may require
language and library capabilities that are not yet listed here. It may also
show that some ideas named today should take a different form. Flecha will keep
the destination clear while allowing the route to improve.

## And beyond

Self-hosting is a beginning, not an endpoint. No post-1.0 item below is a
promise or a fixed definition of 2.0. They describe places Flecha may dare to
go once it owns its toolchain and has a stable foundation.

- A broader self-hosted developer platform: formatter, language server,
  debugger integration, profiler, package ecosystem, documentation tools, and
  reproducible cross-platform builds.
- Deeper compiler independence through faster incremental compilation,
  additional native backends, stronger optimization, and possibly more of the
  linker and toolchain implemented in Flecha.
- First-class native application development through graphics, GPU compute,
  audio, windowing, input, and ergonomic GUI libraries built over APIs such as
  Vulkan.
- A mature service ecosystem with async applications, secure transports,
  HTTP generations, WebSocket, gRPC, observability, and distributed systems
  libraries.
- Targets beyond ordinary desktop and server programs, including embedded and
  bare-metal systems, operating-system components, WebAssembly, and other
  constrained or heterogeneous environments.
- Stronger correctness tools around ownership, concurrency, unsafe boundaries,
  reproducibility, and machine-checkable program contracts.

Flecha 2.0, if and when it exists, should be defined by discoveries made while
building and using 1.x—not by locking today's guesses into tomorrow's language.
