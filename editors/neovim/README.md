# Flecha support for Neovim

This runtime plugin provides `.fl` filetype detection, syntax highlighting,
brace-aware indentation, basic buffer settings, automatic startup of the
Flecha language server, and an optional Codex project chat in a polished right
sidebar.

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
  -- Coalesce rapid edits before notifying the server (default: 150 ms).
  debounce_text_changes = 150,
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

## Codex project chat

Press `<leader>mc` to open a Codex chat on the right side of Neovim. The upper
pane contains the conversation and compact command/edit activity; the lower
pane is a multiline prompt composer. Press `<C-s>` from either insert or normal
mode to send, or press `<CR>` from normal mode. Press `q` from normal mode to
close the panel. In the conversation pane, `i` focuses the composer and `r`
starts a fresh chat. Press `<leader>ml` to list chats created from Neovim for
the current project, then select one to restore its transcript and continue its
Codex thread. The mapping is available inside both Codex panes and globally, so
history can be opened while the panel is closed. This intentionally replaces
an existing normal-mode `<leader>ml` mapping; remap the
`<Plug>(FlechaCodexChats)` target if you prefer another key. The same list is
always available as `:FlechaCodexChats`.

The chat runs `codex exec` from Git's resolved top-level directory, including
when the active file was opened through a symlink. A Longbow project without a
Git repository uses its nearest `longbow.toml` root and opts into Codex's
non-Git mode. A named file outside any recognized project uses its containing
directory as the workspace and also opts into non-Git mode; an unnamed buffer
in an unmarked directory is refused. The chat reuses the account already
authenticated by the Codex CLI, keeps the Codex thread across prompts, and
grants the agent workspace-write access so it can edit project files. Install
the Codex CLI and sign in once before using it:

```sh
codex login
codex login status
```

The plugin never reads or stores authentication tokens itself. It also avoids
unrestricted filesystem access: Codex runs in its workspace-write sandbox and
routes exceptional approval requests through Codex's automatic reviewer.
Modified source buffers are saved before a prompt is sent, and Neovim checks
for agent-written file changes when each turn finishes.

Chat metadata and transcripts are cached under Neovim's data directory rather
than written into the project. The cache is partitioned by canonical project
root and stores up to 50 chats per root; it contains Codex thread IDs but no
authentication tokens. Set `cache_path` or `max_cached_chats` in
`vim.g.flecha_codex` to customize it.

Executable discovery checks Neovim's `PATH`, the standalone installer's
default `~/.local/bin` location, and the Codex binary bundled with the ChatGPT
desktop app. `:FlechaCodexInfo` shows the resolved executable.

Commands are also available as `:FlechaCodex`, `:FlechaCodexNew`,
`:FlechaCodexStop`, `:FlechaCodexChats`, and `:FlechaCodexInfo`. To customize
the panel before plugins load:

```lua
vim.g.flecha_codex = {
  -- Fraction of the editor width, clamped to a comfortable range.
  width = 0.38,
  input_height = 6,
  max_cached_chats = 50,
  -- Override this for a differently installed Codex executable.
  command = { "codex" },
}
```

To choose another mapping, map the plug target before the runtime loads:

```vim
nmap <leader>ai <Plug>(FlechaCodexToggle)
```

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
the Codex panel and JSON event handling, running Longbow from an empty editor,
quickfix navigation, filetype detection, buffer settings, representative syntax
groups, and indentation while typing closing braces.
