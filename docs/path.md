# Lexical paths

Import `lib::std::io::path` for platform-aware lexical path operations. These
operations examine bytes and do not access the filesystem, follow symbolic
links, make a path absolute, or determine whether a path exists.

```flecha
@imports :: lib::std::io::path;

configuration, joinError := path::join("settings", "flecha.conf");
if joinError != none {
    return joinError;
}

cleaned, normalizeError := path::normalize(configuration.view());
if normalizeError != none {
    return normalizeError;
}
```

`join(base, child)` returns a new owning string. It inserts the host path
separator only when needed and does not otherwise normalize either argument.
An empty argument is omitted, and two empty arguments succeed without an
allocation. An absolute child replaces the base. On Windows, a child with its
own drive or UNC volume also replaces the base, including a drive-relative
child such as `D:logs`. Joining a relative child to a bare drive preserves its
drive-relative meaning: joining `C:` and `logs` produces `C:logs`, not
`C:\logs`.

`normalize(value)` allocates a new owning string and applies the host's lexical
rules. It removes repeated separators and `.` components, resolves `..`
against a preceding ordinary component, preserves leading `..` components in
a relative path, and prevents `..` from crossing an absolute root. An empty
path normalizes to `.`. Normalization does not consult the current directory,
resolve links, validate that the result is accepted by the filesystem, or
change relative paths into absolute paths.

Both allocating operations return an empty string with
`AllocationError.OutOfMemory` on failure. Their temporary allocations and
partially built output are released before returning.

`file_name`, `parent`, and `extension` do not allocate. Each returns a
`stringv` into its input and therefore keeps that source borrowed for the
view's lifetime. The source cannot be replaced or destroyed while the result
is still live, and a result cannot escape a local owning source. These
component operations ignore trailing separators but otherwise inspect the
original spelling; call `normalize` first when normalized components are
required.

- `file_name(value)` returns the last non-separator component. A root has no
  filename.
- `parent(value)` returns the bytes before the last component. A root is its
  own parent, and a single relative component has an empty parent.
- `extension(value)` returns the last suffix beginning with `.` in the final
  filename. A leading dot alone does not form an extension, and a trailing dot
  produces no extension.

## Platform rules

POSIX recognizes only `/` as a separator and emits `/`. It treats one or more
leading separators as the single root `/`.

Windows recognizes both `/` and `\` as input separators. Normalization emits
`\`, and joining uses it when a separator must be inserted. It distinguishes
drive-relative paths such as `C:logs` from drive-absolute paths such as
`C:\logs`. A complete UNC `\\server\share` prefix is an indivisible absolute
root, so parent components cannot escape its share. Path spelling is
byte-oriented; these operations do not perform Unicode case folding or compare
volumes.

The module selects the rules used by filesystem operations on the current
host. Its tests exercise both grammars directly on every supported host so the
Windows behavior is not dependent on Windows-only test coverage.
