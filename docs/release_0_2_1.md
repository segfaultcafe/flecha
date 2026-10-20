# Flecha 0.2.1 module-correctness milestone

Flecha 0.2.1 corrects the meaning of modules. Flecha 0.2.0 conflated a source
file with a module, making module identity, visibility, imports, and tests
depend on filenames in ways that were not part of the language model.

This is a correctness fix rather than a new syntax wave.

## Corrected model

A `.fl` file is a **source unit**. A source unit owns:

- its source text and diagnostic locations;
- its module declaration; and
- its own `@imports` bindings.

A **logical module** is the shared semantic scope formed by every source unit
with the same declared module name in the same namespace. Its identity is:

```text
(source identity, namespace path, declared module name)
```

The filename is not part of module identity. For example:

```text
foo.fl  -> module homer;
bar.fl  -> module homer;
```

Both source units contribute to one logical module named `homer`. Importing
`homer` loads both units. Neither `foo` nor `bar` becomes an importable module
because of its filename.

All source units in a logical module share one module scope. A declaration in
either unit can access every module-scope declaration in the other unit,
regardless of declaration visibility. Duplicate top-level names across source
units are module errors; source order never selects a winner.

## Source-local imports

Imports belong to the source unit containing the `@imports` block. They do not
become shared module declarations and are not inherited by sibling source
units:

```flecha
// foo.fl
module homer;

@imports
    :: root::storage;
```

```flecha
// bar.fl
module homer;

// storage is not in scope here unless bar.fl imports it too.
```

Imported declarations remain qualified, are not re-exported, and do not alter
the logical identity of the importing module. Semantically, each source unit
gets a resolution scope containing its import aliases, layered over the shared
logical-module scope. Explicit alias collisions are therefore checked per
source unit, repeated implicit final names form an overlapping namespace, and
declaration collisions are checked across the whole logical module.

## Declaration visibility

Declaration visibility is independent of the source unit in which the
declaration appears:

| Declaration | Reach |
| --- | --- |
| Unmarked | Every source unit in the same logical module |
| `quiver` | The logical module and other modules in the same namespace |
| `export` | The logical module and every importer allowed to import it |

Moving a declaration between source units of the same logical module cannot
change its visibility or nominal identity.

Private structure and enum members remain a separate boundary. They are
accessible only according to the exact owning-type rules and do not become
accessible merely because code is in the same logical module.

## Deprecated private modules

`private module name;` remains accepted for compatibility but produces a
deprecation warning.
It is a module-wide import restriction: the logical module may be imported only
from the same namespace. Every source unit contributing to that logical module
must agree on ordinary versus private module status. Mixing `module homer;` and
`private module homer;` is an error.

`export` always means public declaration visibility. In particular, the parser
must no longer silently convert `export` to `quiver` inside a private module.
The private-module import gate still prevents an outside module from importing
that logical module; `export` does not bypass the gate.

New code should use an ordinary module with explicit `quiver` declarations for
same-namespace APIs:

```flecha
module homer;

quiver void namespace_api() {}
void module_helper() {}
```

Migration from a legacy private module normally means replacing `private
module` with `module` and replacing declarations that relied on downgraded
`export` semantics with `quiver`.

## Overlapping import namespaces

Distinct logical modules with the same declared name may be imported without
aliases. Their accessible declarations overlap under that local name. A member
provided by one origin resolves normally; a member provided by multiple origins
is ambiguous at its use site. Explicit aliases isolate the modules, and imports
of the same canonical logical module are deduplicated.

This overlap changes name lookup only. It never merges module identity, nominal
types, module-private declarations, generic specializations, or error families.

## Logical test modules

Test pairing is based on declared logical module identity, not filenames. A
logical test module named `<name>_test` pairs with logical production module
`<name>` in the same source identity and namespace.

Any number of source units may declare the same test module:

```text
arithmetic_cases.fl  -> module numeric_test;
boundary_cases.fl    -> module numeric_test;
```

Running either test source unit loads the complete `numeric_test` logical
module and discovers tests across all of its source units. Test filenames do
not need an `_test.fl` suffix.

Pairing grants the existing one-way access to the production module's
module-private top-level declarations. It does not grant access to private
structure or enum members, does not re-export privileged declarations, and
does not make imports module-wide. A test source unit must still import every
external namespace it uses.

## Implementation delivered

1. Index module headers by source identity, namespace, and declared module
   name instead of resolving imports directly to `<name>.fl`.
2. Assign one semantic module identifier and one scope to every source unit in
   a logical module.
3. Keep import tables attached to source units while resolving their bodies in
   the shared module scope.
4. Remove the filename/module-name equality requirement.
5. Make `export` unconditional and emit a deprecation diagnostic for `private
   module`.
6. Require consistent private-module status across all source units in a
   logical module.
7. Make compiler and Longbow test discovery inspect declared `_test` module
   names, deduplicate their source units, and generate one harness for each
   complete logical test module.
8. Preserve one nominal identity and native symbol namespace across every
   source unit in the logical module.
9. Allow implicit aliases for distinct same-named imports to bind an overlapping
   local namespace, deduplicate repeated canonical modules, and diagnose member
   ambiguity at the use site. Explicit aliases remain unique and isolated.

## Compatibility boundary

0.2.1 does not add re-exports, cyclic module dependencies, binary module
interfaces, nested module syntax, or implicit sharing of imports.

## Release validation

The parser, loader, semantic, visibility, generic, test-discovery, lowering,
native-linking, and installed-package suites cover:

- two or more source units composing one ordinary logical module;
- module-private access in both directions across source units;
- source-local imports that do not leak between source units;
- stable `export`, `quiver`, and unmarked visibility after moving declarations
  between source units;
- deprecated private logical modules and mixed-status diagnostics;
- globally classified exports inside legacy private modules;
- filename-independent imports and test pairing;
- multiple source units contributing tests to one logical test module;
- distinct same-named imports resolving unique members through one namespace;
  and
- use-site ambiguity diagnostics and explicit-alias isolation for colliding
  members.

All existing 0.2.0 behavior unrelated to these corrected module boundaries
must remain compatible.
