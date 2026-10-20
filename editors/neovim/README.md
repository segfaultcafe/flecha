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

Windows PowerShell installation and upgrade instructions are available in
[`docs/editors.md`](../../docs/editors.md).

## Test

From the repository root:

```sh
make test-neovim
```

The test runs Neovim headlessly and checks filetype detection, buffer settings,
representative syntax groups, and indentation while typing closing braces.
