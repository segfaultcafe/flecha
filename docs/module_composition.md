# Logical module composition (0.2.1 correction)

This document defines the logical module model implemented in Flecha 0.2.1.

## Terms and identity

- A **source unit** is one `.fl` file with a module declaration and its own
  imports.
- A **logical module** is the shared scope formed by every source unit with the
  same declared module name in the same namespace.
- A **namespace** is one directory inside one source identity. Two directories
  or dependencies remain distinct namespaces even when they contain modules
  with the same name.

A logical module is identified by:

```text
(source identity, namespace path, declared module name)
```

The filename is not part of that identity.

## Filenames do not name modules

The module declaration is authoritative:

```text
foo.fl  -> module homer;
bar.fl  -> module homer;
```

Both source units contribute to the same logical module, `homer`. Neither
`foo` nor `bar` is importable merely because it is a filename.

```flecha
module consumer;

@imports
    :: root::homer;
```

This imports the logical module assembled from `foo.fl` and `bar.fl`. Import
discovery scans the target namespace, reads module declarations, and groups
source units by declared module name. A file stem mismatch is not an error.

## Shared module scope

Source units in one logical module:

- contribute declarations to one module scope and module identity;
- can access every module-scope declaration contributed by every sibling
  source unit, regardless of `export`, `quiver`, or unmarked visibility;
- keep their `@imports` bindings source-local;
- are assembled in deterministic canonical-path order;
- cannot declare the same top-level name twice.

A duplicate top-level declaration is a module-assembly error. Flecha does not
use source order to select a winner and does not implicitly form overload sets.

Source-unit boundaries are implementation boundaries, not visibility
boundaries. This rule does not weaken member-level privacy inside structures or
enums: their `private` members retain the exact owning-type rules.

## Stable declaration visibility

Visibility belongs to declarations and has the same meaning in every source
unit:

| Declaration | Visible from sibling source units | Visible from same namespace | Visible outside namespace |
| --- | --- | --- | --- |
| Unmarked | Yes | No | No |
| `quiver` | Yes | Yes | No |
| `export` | Yes | Yes | Yes, when the module is importable |

Moving a declaration between source units of the same logical module cannot
change its visibility, nominal identity, or API-exposure classification.

An exported API may expose only exported nominal types. A quiver API may also
expose quiver-visible nominal types from the same namespace, but not unmarked
module-private types.

## Source-local imports

An import is owned by the source unit that contains it:

```flecha
// foo.fl
module homer;

@imports
    :: root::storage;
```

`storage` is in scope while resolving `foo.fl`, but not while resolving another
`homer` source unit. A sibling source must declare the import itself if it uses
that namespace. Imports are not declarations in the shared module scope and
are never re-exported. Each source unit receives a resolution scope containing
its import aliases over the shared logical-module scope. Explicit alias
conflicts are source-local, repeated implicit aliases overlap, and declaration
conflicts are module-wide.

Import paths address declared logical module names rather than files. Brace and
wildcard imports select logical module names and deduplicate their source units,
so a logical module appears once no matter how many files compose it.

## Deprecated `private module`

`private module homer;` remains supported for source compatibility but emits a
deprecation warning. Privacy is a property of the whole logical module, not an
individual source unit. All source units in one logical module must agree:

```text
foo.fl  -> private module homer;
bar.fl  -> private module homer;
```

Mixing `module homer;` and `private module homer;` is a compile error.

A private logical module can be imported only from its own namespace. However,
`export` always marks a globally visible declaration and is no longer rewritten
to `quiver` by the parser. The private-module import gate still prevents an
outside importer from reaching that export.

New code should normally use an ordinary module and express same-namespace
visibility directly:

```flecha
module homer;

quiver void namespace_api() {}
void module_helper() {}
```

## Tests pair by logical module

Test identity comes from the declared module name rather than the filename. A
logical module named `<name>_test` pairs with `<name>` in the same namespace and
source identity.

Multiple source units may contribute to one test module:

```text
basic_cases.fl     -> module homer_test;
boundary_cases.fl  -> module homer_test;
```

Running either source unit loads the whole `homer_test` module and discovers
tests from both. Test filenames do not require an `_test.fl` suffix.

The existing test privilege remains one-way: the paired test module may access
module-private top-level declarations in the production module, but not private
members of structures or enums. Pairing does not share imports between test
source units and does not implicitly re-export production declarations.

## Overlapping same-named imports

Distinct logical modules may have the same declared name. When a source unit
imports them without explicit aliases, their accessible declarations overlap
under one local namespace:

```flecha
@imports
    :: lib::first::errors
    :: lib::second::errors;

errors::ParseError
errors::NetworkError
```

A member supplied by only one origin resolves normally. A member supplied by
multiple origins is ambiguous at its use site, and the diagnostic lists every
candidate. Declarations unavailable to the importer do not participate in the
ambiguity.

Explicit aliases keep the logical modules separate. An aliased import does not
also contribute to the unaliased namespace, and duplicate explicit aliases in
one source unit remain errors. Reaching the same canonical logical module more
than once is deduplicated rather than treated as overlap.

Overlap affects name lookup only. It does not merge nominal identities,
module-private scopes, generic specializations, or error families.

## Required diagnostics

The compiler must report at least:

- duplicate top-level declarations across source units, with both locations;
- an import whose declared module name does not exist in the target namespace;
- conflicting ordinary and private declarations of one logical module;
- deprecated use of `private module`;
- an import of a private logical module from outside its namespace;
- a public API that leaks a non-exported nominal type;
- ambiguous lookup across overlapping imports, with every candidate origin;
- duplicate explicit source-local aliases; and
- a logical test module without its same-namespace production module.

These rules supersede Flecha 0.2.0's one-file/one-module behavior. The delivered
release contract and validation matrix are recorded in the
[0.2.1 module-correctness milestone](release_0_2_1.md).
