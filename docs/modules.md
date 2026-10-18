# Modules and imports

A module is one `.fl` file. Its declaration comes first and matches the filename:

```flecha
module formatting;

export void print_result(usize index) {
    stdout("Found at index: ");
    stdoutln(index);
}
```

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

Top-level functions, structures, and error types are private to their module unless
prefixed with `export`. For an unsafe exported function, use `export unsafe ...`.
Exporting a structure bundles all its non-private member functions, including its
accessible initializer. Member functions do not take `export` individually.
Existing field visibility remains unchanged. Methods are defined in the structure's
module; importing a structure does not permit adding methods to it.

An exported API may not expose a private type through a signature or public field.
Private fields and private methods can use private types. `tear` is still
compiler-invoked only, regardless of visibility. Ownership contracts, unsafe
permission, constness, borrowing, noncopyability, and immovability are all enforced
across module boundaries.

## Compilation and identity

The compiler loads the reachable source graph, gives each module a separate scope,
and compiles the graph together. The same canonical file reached through a diamond
is loaded once. Different modules may use identical declaration names without
sharing function, structure, or error-family identities. Cycles, missing modules,
filename mismatches, duplicate aliases, and paths escaping a source root are
errors. A file cannot be interpreted against two conflicting source roots.
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
