# Modules and imports

A module is one `.fl` file. Its declaration comes first and matches the filename:

```flecha
module formatting;

export void print_result(usize index) {
    stdout("Found at index: ");
    stdoutln(index);
}
```

A module may be restricted to its quiver:

```flecha
private module formatting;

export void print_result(usize index) {}
```

Only modules in the same quiver may import a private module. Within a private
module, `export` means quiver-wide visibility rather than global visibility;
unmarked declarations remain visible only inside that module.

For `src/ui/formatting.fl`, the import path is
`root::ui::formatting`. Directories supply the namespace path; the module
declaration contains only the filename stem.

## Import blocks

```flecha
module main;

@imports
    :: lib::std::containers::algorithms;

i32 main() {
    [5]i32 values = [10, 20, 30, 20, 40];
    index, found := algorithms::index_of<i32>(values[:], 30);
    if found {
        stdoutln(index);
        return 0;
    }
    return 1;
}
```

The `@imports` block follows the module declaration and precedes other declarations.
One semicolon terminates the entire block. Whitespace and newlines do not delimit
entries; `@imports :: root::math;` is equally valid. The multiline form above is
idiomatic. `root` and `lib` are reserved import-path segments: `:: root` and
`:: lib` start entries, so those words cannot name imported modules or intermediate
directories. Import aliases use `as` and must not collide with another import or
a top-level declaration in the same module.

`root` resolves against the current source root. `lib::<library>::<module path>`
resolves against a configured library's source root. When that library imports
`root::helper`, it resolves its own helper, not the application's helper.
The final path segment becomes the local namespace unless an alias is supplied.
Imported functions use `algorithms::index_of<i32>(values[:], 30)`. Imported
types use `containers::Box`; static methods use `containers::Box::init(...)`. Existing
instance member syntax is unchanged. Qualified error types and variants work as
`containers::ContainerError.Empty`.

Imports expose a namespace rather than injecting declarations into the local
scope. Dependencies' imports are not re-exported. Importing a module does not
execute initialization code.

Directory imports support all immediate modules or selected module names:

```flecha
@imports
    :: root::ui::*
    :: lib::std::containers::{array, algorithms};
```

`*` imports every `.fl` file directly in the directory, in filename order; it does
not recurse into subdirectories. An existing empty directory imports nothing.
Braces select module files, not declarations within a module. Each module keeps
its filename as its namespace, so calls still use `algorithms::contains<i32>(values[:], 2)`.
Duplicate namespaces are errors; use separate explicit imports with `as` to
resolve collisions. A wildcard or brace group cannot take a collective alias.

## Visibility

Top-level declarations have three visibility levels:

```flecha
export void public_api() {}  // every importing module
quiver void shared_api() {}  // modules in the same quiver
void module_api() {}         // this module only
```

For an ordinary `module`, these meanings apply exactly as written. For a
`private module`, both `export` and `quiver` declarations are quiver-visible.
The module itself cannot be imported across its quiver boundary, including by
wildcard imports.

A module is one `.fl` file. A quiver is the set of modules in one immediate
directory under the same source-root or library identity. A nested directory is
a different quiver, and matching directory names in separate roots or libraries
do not share visibility. Directory imports (`::*`) still import every immediate
module, but never widen declaration visibility.

`quiver` and `export` apply only to module-scope declarations. A structure or
enum gives its reach to all non-private fields and methods; member functions
cannot carry either modifier. `private` fields and methods are accessible only
from methods of their exact owning structure or enum. Methods must be defined in
their owner's module, and importing a type does not permit adding methods to it.

An exported API may expose only exported nominal types. A quiver API may expose
exported types and quiver types from the same quiver, but not module-private
types. These checks include free functions, public fields, methods, enums,
errors, callable signatures, generic declarations, and generated nominal
specializations. An exported generic instantiated with a quiver type remains
usable within that quiver but cannot leak through an exported API. Private
fields and private methods may use module-private types. `tear` remains
compiler-invoked only. Ownership contracts, unsafe permission, constness,
borrowing, noncopyability, and immovability are enforced across every boundary.

## Compilation and identity

The compiler loads the reachable source graph, gives each module a separate scope,
and compiles the graph together. The same canonical file reached through a diamond
is loaded once. Different modules may use identical declaration names without
sharing function, structure, or error-family identities. Cycles, missing modules,
filename mismatches, private-module imports from a different quiver, duplicate
aliases, and paths escaping a source root are errors. A file cannot be interpreted
against two conflicting source roots.
Import nesting is limited to 256 levels with a diagnostic rather than unbounded
recursion.

Imported files require a module declaration. Legacy standalone files without one
remain supported, preserving their native symbols. Such files cannot use
`@imports`. `export` is Flecha visibility, not a C ABI export directive.
Separate binary-module interfaces, re-exports, cyclic modules, and FFI linkage
contracts are not part of this implementation.

```sh
flecha check src/main.fl --root src
flecha build src/main.fl --root src -o program
flecha build src/main.fl --root src --lib std=../standard_library/src -o program
```

Without `--root`, the compiler uses the nearest ancestor directory named `src`,
or the entry file's directory if none exists. `--lib` explicitly maps library names
to source directories; it does not download or invent a standard library.
Diagnostics include the originating source file and line/column. Compiler and
ownership errors stop before object emission. An executable's entry module needs
`i32 main()` or `void main()`; a void entry returns process status zero.

Neovim highlights `@imports` in light blue, and recognizes `module` and `export`.
See [Longbow](longbow.md) for project creation and builds and
[the module example](../examples/modules/src/main.fl) for a runnable project.
