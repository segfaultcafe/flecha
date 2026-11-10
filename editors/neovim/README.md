# Flecha support for Neovim

This runtime plugin provides `.fl` filetype detection, syntax highlighting,
brace-aware indentation, basic buffer settings, and automatic startup of the
Flecha language server.

Callable declarations and capture lists are highlighted in examples such as:

```flecha
func Mapper(i32) => (i32);
Mapper doubled = [](i32 value) => (i32) { return value * 2; };
Mapper offset = [copy amount](value) => { return value + amount; };
Mapper nested = [own offset](value) => { return offset(value) + 1; };
```

## Install

From an extracted Flecha release archive on Linux or macOS, copy the bundled
runtime plugin into Neovim's native package path:

```sh
destination="${XDG_DATA_HOME:-$HOME/.local/share}/nvim/site/pack/flecha/start/flecha.nvim"
mkdir -p "$(dirname "$destination")"
cp -R /path/to/flecha/share/flecha/editors/neovim "$destination"
```

When copying the runtime out of the release tree, install the release's `bin`
directory on `PATH` or set the LSP overrides below. A symlink to the runtime in
the extracted release preserves its relative link to `bin` and needs no path
configuration.

For a development checkout, link this directory instead:

```sh
mkdir -p ~/.local/share/nvim/site/pack/flecha/start
ln -s /path/to/flecha/editors/neovim \
    ~/.local/share/nvim/site/pack/flecha/start/flecha.nvim
```

Restart Neovim and open a `.fl` file. `:set filetype?` should report
`filetype=flecha`. Enable indentation plugins with `filetype plugin indent on`
if your configuration does not already do so. Closing braces align with their
opening block; indentation width follows your `shiftwidth` setting.

Plugin managers that support local directories can use `editors/neovim` as a
local plugin source instead.

## Language server

On Neovim 0.8 or newer, opening a Flecha buffer automatically starts
`flecha-lsp`. Diagnostics, hover, definition, references, completion, and
rename then use Neovim's built-in LSP client. Semantic-token highlighting is
available on Neovim 0.9 or newer. The syntax and indentation parts of the
plugin remain usable on older Neovim versions.

The plugin first looks for a complete relocatable release pair at
`bin/flecha-lsp` and `bin/flecha` relative to the bundled runtime. If that pair
is not present, it resolves `flecha` on `PATH` and prefers the sibling
`flecha-lsp` from the same directory. This prevents an obsolete standalone
server earlier on `PATH` from being mixed with the selected compiler. The
resolved compiler is passed to the server as `FLECHA_COMPILER`.

You can override either command before plugins are loaded:

```lua
vim.g.flecha_lsp_cmd = {
  "/path/to/flecha-lsp",
  -- Optional additional server arguments go here.
}
vim.g.flecha_compiler_path = "/path/to/flecha"
```

For a development checkout, typical paths are
`build/src/lsp/flecha-lsp` and `build/flecha`. To configure through a plugin
manager instead, call the module's setup function:

```lua
require("flecha.lsp").setup({
  cmd = { "/path/to/flecha-lsp" },
  compiler_path = "/path/to/flecha",
  -- Optional: capabilities, init_options, settings, on_attach, root_markers.
})
```

The nearest enclosing `longbow.toml` or `.git` directory is used as the
workspace root. Buffers in the same root share one language-server client,
including a compatible Flecha client started elsewhere in your configuration.
Use `:FlechaLspInfo` to inspect the resolved paths and root.

To keep only syntax, indentation, and Longbow commands, disable automatic LSP
startup before the plugin loads:

```lua
vim.g.flecha_lsp_autostart = 0
```

Example key mappings for the built-in client:

```lua
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.keymap.set("n", "gr", vim.lsp.buf.references)
vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename)
```

Insert-mode completion is available through `<C-x><C-o>`; completion plugins
can use the same LSP client.

## Run projects and navigate errors

Press `<leader>rr` anywhere inside a Longbow project to run `longbow run` and
open the results in Neovim's quickfix window. This works immediately after
starting Neovim at the project root, even when no file is open. From a named
buffer, the nearest enclosing `longbow.toml` is used and the file is saved
first. Compiler diagnostics are parsed as file, line, and column locations.
Move the cursor onto a result and press `<leader>fe` to open that exact source
line. Enter on the result performs the same exact-line jump. You can also run
the workflow with `:LongbowRun`.

Press `<leader>rt` to run `longbow test <project-root> --verbose` for the same
enclosing project and open the test output in quickfix. The command form is
`:LongbowTest`. On failure, `<leader>fe` opens the selected failed test's
declaration. If the cursor is on summary output instead, it opens the first
navigable failure.

To choose a different run mapping, map `<Plug>(FlechaLongbowRun)` in your
Neovim configuration before the Flecha runtime plugin loads:

```vim
nmap <leader>br <Plug>(FlechaLongbowRun)
nmap <leader>bt <Plug>(FlechaLongbowTest)
```

Windows PowerShell installation and upgrade instructions are available in
[`docs/editors.md`](../../docs/editors.md).

## Test

From the repository root:

```sh
make test-neovim
```

The test runs Neovim headlessly and checks LSP discovery/startup/client reuse,
running Longbow from an empty editor, quickfix navigation, filetype detection,
buffer settings, representative syntax groups, and indentation while typing
closing braces.
