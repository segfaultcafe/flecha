# Flecha 0.3.2 analyzer and correctness release

Flecha 0.3.2 adds the compiler-side analysis service used by Flecha language
servers and fixes several correctness and performance defects discovered while
integrating live editor diagnostics.

## Analyzer protocol

`flecha analyze --stdio` now runs a long-lived analyzer over standard input
and output. It accepts framed JSON messages for opening, changing, analyzing,
and closing versioned source documents. Unsaved editor contents are layered
over the project files without modifying the workspace on disk.

The analyzer resolves the project's Longbow package graph, so diagnostics for
standard-library and registry imports use the same module roots as a normal
project build. The protocol is an internal compiler/server boundary; editor
clients should communicate through `flecha-lsp` rather than depending on it
directly.

The bundled Visual Studio Code extension can start `flecha-lsp` and locate the
compiler. Neovim keeps its existing editor integration and gains verbose
Longbow test commands with quickfix navigation.

## Correctness and performance fixes

- Allocation analysis summarizes verified resource helpers instead of
  repeatedly analyzing the same layered call graph. Projects affected by the
  regression move from exponential compilation back to bounded analysis.
- Borrow origins propagate through tuple results, preventing checked
  references inside aggregates from escaping their valid scope.
- A missing closing brace before a top-level declaration produces a focused
  parser diagnostic.
- Diagnostics use stable source-level names for missing enum and error
  variants instead of internal module-qualified names.

## Upgrade

Install the 0.3.2 compiler and matching VS Code extension. Flecha source and
Longbow manifests compatible with 0.3.x do not require changes.

`flecha-lsp` requires this release or newer because Flecha 0.3.1 does not
provide `analyze --stdio`. Configure the language server with the path to the
0.3.2 `flecha` executable when it is not available on `PATH`.

## Validation

The release suite covers analyzer framing, document versioning, unsaved source
overlays, package-graph resolution, diagnostics, and the complete stdio
document lifecycle. Existing compiler, standard-library, sanitizer, clean
installation, native packaging, and editor-extension checks remain release
gates on Linux, Windows, and both Intel and Apple Silicon macOS.
