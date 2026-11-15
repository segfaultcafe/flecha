# Flecha for Visual Studio Code

This extension recognizes `.fl` files, supplies Flecha-aware comments and
brackets, highlights the language syntax implemented by the compiler, and
starts the Flecha language server for diagnostics and semantic editor features.

Callable declarations and capture lists are highlighted in examples such as:

```flecha
func Mapper<T>(T) => (T);
Mapper doubled = [](i32 value) => (i32) { return value * 2; };
Mapper offset = [copy amount](value) => { return value + amount; };
Mapper nested = [own offset](value) => { return offset(value) + 1; };
```

## Install a release

Download `flecha-language-VERSION.vsix` from the matching Flecha GitHub Release,
then run this command on Linux, macOS, or Windows:

```sh
code --install-extension flecha-language-VERSION.vsix
```

Restart any open Flecha editor windows after upgrading. Confirm the installed
version with `code --list-extensions --show-versions`; the extension identifier
is `flecha.flecha-language`.

The extension first looks for `flecha-lsp` and `flecha` in the matching Flecha
release bundle. This lookup is relative to the installed editor support, so an
extracted release remains usable after it is moved. A separately installed
VSIX resolves `flecha` on `PATH` and selects the sibling `flecha-lsp` from the
same directory. This prevents an obsolete standalone server earlier on `PATH`
from shadowing the current compiler bundle. Override either executable in VS
Code settings when developing from a checkout or when the tools are installed
elsewhere:

```json
{
  "flecha.server.path": "/path/to/flecha-lsp",
  "flecha.compiler.path": "/path/to/flecha"
}
```

Use **Flecha: Restart Language Server** after replacing either executable.

If the `code` command is unavailable, open the Extensions view, choose **Views
and More Actions**, select **Install from VSIX**, and select the downloaded
file.

## Package from a checkout

Node.js 22 or newer is required. From the repository root, run:

```sh
make package-vscode
code --install-extension editors/vscode/flecha-language-VERSION.vsix
```

The Make target uses the checked-in npm lockfile, tests relocatable tool
discovery, creates the VSIX with the pinned official packager, and verifies its
installation manifest and contents. The extension version intentionally
matches the Flecha project version.

The language server uses full-document synchronization and provides compiler
diagnostics, hover information, go to definition, references, completion,
rename, and semantic tokens. Semantic queries currently cover the entry module
and the modules it imports; workspace-wide indexing and incremental analysis
remain future work.

See [`docs/editors.md`](../../docs/editors.md) for release checksums, upgrading,
uninstalling, and Neovim installation.
