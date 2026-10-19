# Flecha for Visual Studio Code

This extension recognizes `.fl` files, supplies Flecha-aware comments and
brackets, and highlights the language syntax implemented by the compiler.

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

The v0.1 extension provides grammar-based editing support only. Language-server
features such as completion, navigation, and semantic tokens are future work.

See [`docs/editors.md`](../../docs/editors.md) for release checksums, upgrading,
uninstalling, and Neovim installation.
