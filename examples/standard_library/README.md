# Flecha 0.3 standard-library API tour

This installed-toolchain example is the executable companion to the
[Flecha 0.3 standard-library reference](../../docs/standard_library.md). Its
entry module imports every documented public module through the package alias
`lib::std`; building it therefore verifies that the documented package layout
is present. The program then exercises deterministic byte, text, collection,
math, encoding, hashing, cryptography, and random-generator operations.

From a source checkout:

```sh
longbow test
longbow run
```

The program prints:

```text
standard library API tour: ok
```

The release smoke suite copies these sources into a clean project with no
source-tree dependency, then builds, tests, and runs it using only a relocated
Flecha installation. Topic-specific behavior, including blocking I/O and
platform boundaries that do not belong in a deterministic tour, is covered by
the focused examples and tests linked from the reference's module index.
