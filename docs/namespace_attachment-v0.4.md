# Namespace attachment for Flecha 0.4

Status: accepted language contract; v0.4 release blocker; not implemented

Namespace attachment lets a source unit live in a physical submodule folder
while contributing to a logical module in an ancestor namespace. It preserves
the existing multifile module model without forcing the public namespace to
mirror every implementation directory.

Flecha 0.4 is not complete until namespace attachment is implemented through
the module loader, parser, semantic model, diagnostics, analyzer, and tests.

## Motivation

A micro library can usually keep one small directory and one obvious public
namespace. A super library such as `std` contains many independently growing
sublibraries. Keeping every source unit directly in one directory eventually
makes that directory difficult to navigate, while exposing every physical
subdirectory as a public namespace makes imports reflect implementation
details.

Namespace attachment separates those concerns:

- physical folders organize implementation ownership and context;
- logical namespaces organize module identity, visibility, and public API;
- the attachment remains constrained by the physical ancestry of the source
  unit, so the logical tree cannot be assembled arbitrarily.

## Source syntax

An attachment follows the module declaration and precedes imports and other
declarations:

```flecha
module gee;
under namespace root::foo;

@imports :: root::support;
```

A source unit may contain at most one `under namespace` declaration. The
declaration is invalid in a legacy standalone source without a module
declaration.

`under` and `namespace` are reserved in this header position. The target is a
canonical namespace path, not an import alias.

## Physical and effective namespaces

Every source unit has:

- a **physical namespace**, derived from its directory relative to its source
  root;
- an optional **attachment namespace**, declared by `under namespace`;
- an **effective namespace**, which is the attachment namespace when present
  and the physical namespace otherwise; and
- a declared module name.

The canonical logical-module identity is:

```text
(source identity, effective namespace, declared module name)
```

Without attachment:

```text
canonical module = physical namespace + declared module name
```

With attachment:

```text
canonical module = attachment namespace + declared module name
```

The filename and intermediate physical submodule folders are not part of the
canonical identity.

## Ancestor restriction

The attachment namespace must be a strict ancestor of the source unit's
physical namespace within the same source identity.

For a file under physical namespace `root::foo::baz`, these are valid:

```flecha
under namespace root::foo;
under namespace root;
```

These are invalid:

```flecha
under namespace root::other;      // Sibling namespace.
under namespace root::foo::qux;   // Not an ancestor.
under namespace lib::other::foo;  // Different source identity.
```

The target cannot contain `.` or `..`, name an imported library, use a
source-local alias, or escape the current package source root. Symbolic paths
are validated after the compiler's normal source-root canonicalization.

## Submodule folders and multifile modules

Consider this source tree:

```text
foo/
├── bar.fl
├── bar2.fl
└── baz/
    ├── bar_extra.fl
    ├── gee.fl
    └── parser.fl
```

The direct source units can compose `root::foo::bar` as they do today:

```flecha
// foo/bar.fl and foo/bar2.fl
module bar;
```

A source in the `baz` submodule folder can contribute to that same logical
module:

```flecha
// foo/baz/bar_extra.fl
module bar;
under namespace root::foo;
```

All three source units have the canonical identity `root::foo::bar` and form
one multifile module. Existing rules for shared declarations, overload sets,
duplicate declarations, deterministic assembly, and source-local imports
continue to apply.

The same physical folder can attach a different module to the ancestor:

```flecha
// foo/baz/gee.fl
module gee;
under namespace root::foo;
```

Its canonical identity is `root::foo::gee`. Without the attachment it would be
`root::foo::baz::gee`.

Files without attachment remain in their physical namespace:

```flecha
// foo/baz/parser.fl
module parser;
```

This module remains `root::foo::baz::parser` and can serve as an implementation
detail of the sublibrary.

## Imports and discovery

Consumers import canonical logical modules without knowing their physical
source locations:

```flecha
@imports :: root::foo::{bar, gee};
```

Brace imports select attached and direct modules alike. A wildcard import
selects every logical module whose effective namespace is the target namespace
and remains nonrecursive with respect to logical namespaces.

Path-only lookup cannot implement this rule because `root::foo::gee` may have
no source unit directly inside `foo/`. Before resolving imports, the module
loader must build an index by reading source headers beneath each configured
source root:

```text
canonical logical module -> ordered contributing source units
```

Indexing needs only enough parsing to obtain module status, module name, and
the optional attachment. Source units are assembled in deterministic canonical
physical-path order after indexing. Reaching the same canonical module through
multiple import paths is deduplicated as it is today.

## Visibility and quivers

The effective namespace determines quiver membership. A module attached under
`root::foo` behaves for visibility as a module declared directly in
`root::foo`, even when its source units live in deeper submodule folders.

An attached source unit can therefore use:

- every declaration in its own logical module, including unmarked declarations
  contributed by sibling source units;
- `quiver` and `export` declarations available from other logical modules in
  its effective namespace; and
- its own source-local import aliases.

Attachment does not expose unmarked declarations from another logical module,
does not widen private member access, and does not inherit imports from another
source unit. It changes module placement, not the existing declaration and
member privacy rules.

Moving a source unit into a submodule folder while preserving its effective
namespace and canonical module identity must not change nominal type identity,
generic specialization identity, exported symbols, or declaration visibility.

## Tests and tooling

Logical test pairing uses the effective namespace. An attached `bar_test`
module pairs with `bar` in the attachment namespace and receives the same
one-way test access as a directly located test module.

Diagnostics, analyzer navigation, rename, hover, and symbol search must report
the physical source location while using the canonical logical identity for
module and symbol resolution. Tools should expose both locations when that
distinction helps navigation.

## Required diagnostics

The compiler must diagnose at least:

- an attachment target that is not a strict physical ancestor;
- an attachment to another source identity or configured library;
- an attachment that escapes the source root;
- duplicate or misplaced `under namespace` declarations;
- use without an ordinary module declaration;
- inconsistent ordinary and legacy private-module status among source units
  that resolve to one canonical logical module;
- duplicate declarations contributed from direct and attached source units,
  with both physical locations;
- an import whose indexed canonical module does not exist; and
- an attached test module without its production module in the effective
  namespace.

## Compatibility

Source units without `under namespace` retain the current module identity,
quiver membership, import behavior, and diagnostics. Namespace attachment adds
no runtime representation and has no direct lowering or LLVM ABI effect.

The feature does change module discovery from target-directory scanning to a
source-root module index. The index must preserve deterministic assembly and
must not make unreachable declarations part of code generation merely because
their headers were discovered.

## V0.4 completion requirements

Namespace attachment is complete only when all of the following are delivered:

- tokenizer, parser, AST, source ranges, and formatter/editor highlighting;
- source-root header indexing and canonical module discovery;
- semantic identity, visibility, quiver, test-pairing, and import resolution;
- deterministic direct-and-attached multifile assembly;
- diagnostics for every invalid boundary above;
- analyzer navigation and symbol identity across physical and logical paths;
- positive and negative fixtures for applications, configured libraries,
  wildcard and brace imports, deep submodule folders, and multifile merges;
- regression coverage proving unchanged behavior for sources without
  attachment; and
- documentation of the resulting standard-library organization.

The [Idiomatic Flecha Guideline](idiomatic_flecha.md) defines when libraries
should use this language mechanism.
