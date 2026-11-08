# Flecha for Visual Studio Code

This extension recognizes `.fl` files, supplies Flecha-aware comments and
brackets, highlights the language syntax implemented by the compiler, and
starts the Flecha language server for live diagnostics.

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

The extension runs `flecha-lsp` from `PATH` and asks it to use `flecha` as its
compiler. Override either executable in VS Code settings when developing from
a checkout:

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

The Make target uses the checked-in npm lockfile, creates the VSIX with the
pinned official packager, and verifies its installation manifest and contents.
The extension version intentionally matches the Flecha project version.

The language server currently provides compiler diagnostics with full-document
synchronization. Completion, navigation, and semantic tokens remain future
work.

See [`docs/editors.md`](../../docs/editors.md) for release checksums, upgrading,
uninstalling, and Neovim installation.
