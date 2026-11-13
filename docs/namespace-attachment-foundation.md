# Namespace attachment foundation

This foundation introduces the optional header clause:

```flecha
module gee;
under namespace root::foo;
```

The module name remains a single identifier. The attachment path names a
logical namespace and does not replace the physical source path. Without the
clause, module loading is unchanged.

## Implemented contract

- `root` and any existing ancestor directory of the source file are valid
  attachment targets.
- Sibling, descendant, unrelated, missing, and non-`root` targets are rejected
  at the clause range with the physical file retained in the diagnostic.
- Explicit imports discover attached sources below the source root.
- Attached sources compose with ordinary or other attached sources that share
  the same logical namespace and module name.
- Physical source ordering is deterministic.
- Declarations use the attached namespace for logical module and quiver access,
  while each source `Module` retains its physical file and physical quiver.
- Exported and quiver-visible members follow the target namespace; private
  members remain inaccessible across modules.

## Deliberately deferred

Wildcard import expansion still enumerates modules physically present in the
selected directory. `ModuleTests.DISABLED_DiscoversAttachedModulesThroughWildcardImports`
and its Flecha fixture delimit the follow-up: wildcard expansion should consume
the same attachment-aware index as explicit imports.

The current per-load attachment index scans each source root once and
prefilters files before parsing their headers. A persistent/incremental index,
including discovery of overlay-only files that do not yet exist on disk, is
deferred. Entry overlays and diagnostics continue to retain physical source
identity through the existing source-provider path.

This document describes a frontend/loader foundation, not completion of the
namespace-attachment feature.
