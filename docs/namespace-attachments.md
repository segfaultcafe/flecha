# Namespace attachments

Flecha source files may attach a module to an ancestor namespace while
retaining their physical source identity:

```flecha
module gee;
under namespace root::foo;
```

The module name remains a single identifier. The attachment path controls
logical ownership and resolution; diagnostics, editor queries, and source
locations continue to use the physical file.

## Contract

- `root` and any existing ancestor directory of the source file are valid
  attachment targets.
- Sibling, descendant, unrelated, missing, escaping, and non-`root` targets
  are rejected at the attachment clause with the physical file retained in
  the diagnostic.
- Explicit and wildcard imports use the same attachment-aware source index.
  A source is discovered once by physical identity even when it contributes
  to a different logical namespace.
- Attached sources compose with ordinary or attached sources sharing the same
  logical namespace and module name.
- Discovery and resulting diagnostics are ordered deterministically by
  physical path.
- Each source root has independent index state. Attachments in a dependency
  package are resolved relative to that package's source root and cannot leak
  into another package.
- Exported and quiver-visible members follow logical ownership. Private
  members remain inaccessible across modules.

## Incremental analysis

`AnalysisSession` owns the source index for its lifetime. The first discovery
of a root indexes attachment candidates once and ordinary directories lazily;
later analyses reuse that state instead of rescanning the source root.

Accepted document open and change events refresh only the affected physical
path. Overlay enumeration lets a newly opened source participate in discovery
even if the file does not exist on disk. Close refreshes the same path after
removing the overlay, so the index either falls back to saved content or drops
the source without leaving a ghost entry. Filesystem watchers can call
`AnalysisSession::Refresh` for disk create, change, and remove events.

The command-line loader uses the same index implementation with load-scoped
ownership, preserving the behavior of callers that do not maintain a long-
lived analysis session.
