# Editor installation

Flecha releases provide a ready-to-install Visual Studio Code extension and a
Neovim runtime plugin. Both provide syntax highlighting, filetype detection,
comments, brackets, indentation support, and automatic `flecha-lsp` startup
for live compiler diagnostics and semantic language features.

Both editors render the `!` in a crashable declaration or call, such as
`load!()`, in orange. Logical negation such as `!ready` keeps the ordinary
operator style. The terminal `crash(...)` builtin is blue to distinguish it
from ordinary function calls.

## Visual Studio Code

Download `flecha-language-VERSION.vsix` and `SHA256SUMS` from the matching
[Flecha GitHub Release](https://github.com/alexbsec/flecha/releases). Verify the
download against `SHA256SUMS`, then install it on Linux, macOS, or Windows:

```sh
sha256sum -c SHA256SUMS --ignore-missing
code --install-extension flecha-language-VERSION.vsix
```

On macOS, use `shasum -a 256 flecha-language-VERSION.vsix`; on Windows, use
`Get-FileHash flecha-language-VERSION.vsix -Algorithm SHA256` and compare the
reported value with `SHA256SUMS`.

The command replaces an older version of the same extension. Restart open
Flecha editor windows after an upgrade. Confirm the installed version with:

```sh
code --list-extensions --show-versions
```

The expected identifier is `flecha.flecha-language`. Remove it with:

```sh
code --uninstall-extension flecha.flecha-language
```

When `code` is not on `PATH`, open the Extensions view, choose **Views and More
Actions**, select **Install from VSIX**, and select the downloaded file.

The language-server client automatically uses `flecha-lsp` and `flecha` from a
matching compiler bundle when the extension is loaded from
`share/flecha/editors/vscode`. The lookup is relative to that directory, so the
bundle can be moved after extraction. A separately installed VSIX resolves
`flecha` on `PATH` and selects the sibling `flecha-lsp` from the same directory,
so an obsolete standalone server cannot shadow the matching compiler. Custom
development builds can be selected in user settings:

```json
{
  "flecha.server.path": "/path/to/flecha-lsp",
  "flecha.compiler.path": "/path/to/flecha"
}
```

Run **Flecha: Restart Language Server** after changing or rebuilding either
executable. Set `flecha.trace.server` to `messages` or `verbose` when debugging
the LSP connection.

The server uses full-document synchronization and provides compiler
diagnostics, hover information, go to definition, references, completion,
rename, and semantic tokens. Semantic queries operate over the entry module and
the modules it imports; workspace-wide indexing and incremental analysis are
not yet implemented.

### Package a checkout

Install Node.js 22 or newer, then run this from the repository root:

```sh
make package-vscode
```

This uses `npm ci` and the checked-in lockfile, tests relocatable bundle and
`PATH` discovery, and packages the extension with the pinned official
`@vscode/vsce` release. It writes
`editors/vscode/flecha-language-VERSION.vsix` and verifies the archive's
identity, version, installation manifest, grammar, and development-file
exclusions, including the bundled language-client entry point. CI performs the
same steps, and the release workflow checks that the extension version matches
the release tag before publishing it.

## Neovim

The compiler archives contain the plugin at
`share/flecha/editors/neovim`. On Linux or macOS, install it as a native package:

```sh
destination="${XDG_DATA_HOME:-$HOME/.local/share}/nvim/site/pack/flecha/start/flecha.nvim"
mkdir -p "$(dirname "$destination")"
cp -R /path/to/flecha/share/flecha/editors/neovim "$destination"
```

On Windows, run the equivalent command in PowerShell:

```powershell
$destination = "$env:LOCALAPPDATA\nvim-data\site\pack\flecha\start\flecha.nvim"
New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
Copy-Item -Recurse C:\path\to\flecha\share\flecha\editors\neovim $destination
```

Restart Neovim and open a `.fl` file. `:set filetype?` should report
`filetype=flecha`. If needed, enable filetype plugins and indentation in the
Neovim configuration:

```vim
filetype plugin indent on
```

On Neovim 0.8 or newer, the plugin automatically starts `flecha-lsp` and
enables diagnostics, hover, definition, references, completion, and rename.
Semantic-token highlighting requires Neovim 0.9 or newer. When the plugin is
copied into Neovim's package directory, put the release's `bin` directory on
`PATH`; the plugin resolves `flecha` first and selects its sibling
`flecha-lsp`. Alternatively, configure their absolute paths before plugins
load:

```lua
vim.g.flecha_lsp_cmd = { "/path/to/flecha-lsp" }
vim.g.flecha_compiler_path = "/path/to/flecha"
```

The nearest enclosing `longbow.toml` or `.git` directory becomes the workspace
root. Use `:FlechaLspInfo` to inspect the resolved configuration. Set
`vim.g.flecha_lsp_autostart = 0` before plugin loading to retain only syntax,
indentation, and Longbow commands.

In a Longbow project, `<leader>rr` runs `longbow run` and opens its output in
the quickfix window. It is available immediately after starting Neovim at the
project root, even with no file open. A named buffer is saved first and its
nearest enclosing `longbow.toml` is used. Diagnostics include their file, line,
and column. Select an item and press `<leader>fe` to open its exact source line.
Enter performs the same exact-line jump. The command form is `:LongbowRun`.
Use `<leader>rt` (or `:LongbowTest`) to run
`longbow test <project-root> --verbose` and place verbose test output in the
same quickfix workflow. On failure, `<leader>fe` opens the selected failed
test declaration, or the first navigable failure when a summary line is
selected.

To upgrade, replace the existing `flecha.nvim` directory with the directory
from the new release. To uninstall, remove that `flecha.nvim` directory.

For a development checkout, use a symbolic link instead of copying:

```sh
mkdir -p ~/.local/share/nvim/site/pack/flecha/start
ln -s /path/to/flecha/editors/neovim \
    ~/.local/share/nvim/site/pack/flecha/start/flecha.nvim
```

Run `make test-neovim` from a checkout to exercise language-server discovery
and reuse, filetype detection, syntax groups, buffer settings, and indentation
headlessly.
