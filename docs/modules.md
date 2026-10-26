# Modules and imports

Beginning with Flecha 0.2.1, a module is a logical scope assembled from source
units, not a single file. Every `.fl` file declares the logical module it
contributes to, and the filename does not determine module identity.

A logical module is identified by its source identity, namespace directory,
and declared module name. For example, both files below contribute to the same
`formatting` module:

```flecha
// text.fl
module formatting;

void prefix() {
    stdout("Found at index: ");
}

export void print_result(usize index) {
    prefix();
    stdoutln(index);
}
```

```flecha
// numbers.fl
module formatting;

export void print_missing() {
    prefix();
    stdoutln("none");
}
```

Importing `root::ui::formatting` loads every source unit declaring
`module formatting;` directly under `src/ui`. Neither `text` nor `numbers` is
importable merely because it is a filename.

All source units in one logical module share declarations and nominal identity.
Unmarked, `quiver`, and exported module declarations are visible to sibling
source units in both directions. Duplicate top-level names across those units
are errors. Private structure and enum members retain their exact owning-type
boundary.

## Import blocks are source-local

```flecha
module main;

@imports
    :: lib::std::containers::collections;

i32 main() {
    [5]i32 values = [10, 20, 30, 20, 40];
    index, found := collections::index_of<i32>(values[:], 30);
    if found {
        stdoutln(index);
        return 0;
    }
    return 1;
}
```

The `@imports` block follows the module declaration and precedes other
declarations. One semicolon terminates the block. Whitespace and newlines do
not delimit entries; `@imports :: root::math;` is equally valid.

Imports belong to the source unit that declares them. Another source unit in
the same logical module inherits the shared module declarations, but it does
not inherit import aliases and must repeat any external imports it uses.
Dependencies' imports are not re-exported.

`root` resolves against the current source root. A library import of the form
`lib::<library>::<namespace>::<module>` resolves against that configured
library's source root. When a library imports `root::helper`, it resolves its
own helper rather than the application's helper.

The declared module name becomes the local namespace unless an alias is
supplied with `as`. Imported functions, types, static methods, and error
variants remain qualified, for example `collections::index_of<i32>(...)`,
`containers::Box`, and `errors::IoError.NotFound`.

Directory imports select declared logical module names:

```flecha
@imports
    :: root::ui::*
    :: lib::std::containers::{array, algorithms};
```

`*` discovers every logical module declared directly in the directory and does
not recurse. Braces select module names, not declarations or filenames. A
logical module appears once even when several files contribute to it, and a
repeated import of the same canonical module is deduplicated.

## Overlapping same-named imports

Distinct modules may share a declared name. Without explicit aliases, their
accessible declarations overlap under one local namespace:

```flecha
@imports
    :: lib::parser::errors
    :: lib::network::errors;

errors::ParseError
errors::ConnectionError
```

A member supplied by only one origin resolves normally. If multiple origins
supply the requested member, the use is ambiguous and the diagnostic lists the
candidates. Explicit aliases isolate the namespaces:

```flecha
@imports
    :: lib::parser::errors as parser_errors
    :: lib::network::errors as network_errors;
```

Overlap affects lookup only; it never merges nominal types, generic
specializations, error families, or module-private scopes. Duplicate explicit
aliases remain errors.

## Visibility

Top-level declarations have three visibility levels:

```flecha
export void public_api() {}  // every allowed importer
quiver void shared_api() {}  // modules in the same quiver
void module_api() {}         // this logical module only
```

| Declaration | Sibling source units | Same quiver | Outside quiver |
| --- | --- | --- | --- |
| Unmarked | Yes | No | No |
| `quiver` | Yes | Yes | No |
| `export` | Yes | Yes | Yes, when the module is importable |

A quiver is the set of logical modules in one immediate directory under the
same source-root or library identity. Nested directories and matching paths in
separate roots are different quivers. Wildcard imports never widen declaration
visibility.

An exported API may expose only exported nominal types. A quiver API may also
expose quiver-visible nominal types from the same quiver, but not unmarked
module-private types. These checks cover functions, fields, methods, enums,
errors, callable signatures, generics, and generated specializations.

## Deprecated private modules

`private module name;` remains accepted for compatibility but emits a
deprecation warning. It is a module-wide import gate: all source units in the
logical module must use `private module`, and only importers in the same quiver
may import it. Mixing ordinary and private declarations of one logical module
is an error.

`export` always retains public declaration visibility, including inside a
legacy private module. It no longer becomes `quiver` implicitly; the module's
import gate is what prevents outside access. New code should prefer an ordinary
module with explicit `quiver` declarations.

## Logical test modules

Tests pair by declared module name rather than filename. A logical module named
`<name>_test` pairs with `<name>` in the same namespace and source identity.
Any number of arbitrarily named source units may contribute tests:

```text
arithmetic_cases.fl  -> module numeric_test;
boundary_cases.fl    -> module numeric_test;
```

Running either source unit with `flecha test` loads the complete test module but
generates a harness containing only tests declared in the requested file.
Longbow discovers and runs every source file declaring a `_test` module so a
project-wide run still executes the complete suite. Imports remain source-local
in test modules too.

The paired test module can access module-private top-level declarations in its
production module. That privilege does not expose private structure or enum
members and does not re-export production declarations.

## Compilation and identity

The compiler loads the reachable logical-module graph and compiles it together.
Different logical modules retain distinct function, structure, generic, and
error-family identities even when their declared names match. Cyclic imports,
missing declared modules, mixed private-module status, paths escaping a source
root, and conflicting source roots are errors. Import nesting is limited to 256
levels with a diagnostic.

```sh
flecha check src/main.fl --root src
flecha build src/main.fl --root src -o program
flecha build src/main.fl --root src --lib std=../standard_library/src -o program
```

Without `--root`, the compiler uses the nearest ancestor directory named `src`,
or the entry file's directory if none exists. `--lib` maps a library name to a
source directory. Diagnostics preserve the originating source file and
line/column.

Legacy standalone entry files without a module declaration remain supported,
but imported source units require one and standalone files cannot use
`@imports`. Separate binary-module interfaces, re-exports, cyclic modules, and
FFI linkage contracts remain outside this implementation.

See [Logical module composition](module_composition.md) for the precise 0.2.1
contract, [Longbow](longbow.md) for project workflows, and
[the module example](../examples/modules/src/main.fl) for a runnable project.
