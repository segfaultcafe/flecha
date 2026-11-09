# Flecha support for Neovim

This runtime plugin provides `.fl` filetype detection, syntax highlighting, and
brace-aware indentation, and basic buffer settings for the currently implemented Flecha language.

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

The test runs Neovim headlessly and checks running Longbow from an empty editor,
quickfix navigation, filetype detection, buffer settings, representative syntax
groups, and indentation while typing closing braces.
