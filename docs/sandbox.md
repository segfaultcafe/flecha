# Docker sandbox

The development sandbox provides a disposable Linux environment with the
current checkout of Flecha installed. It includes:

- `flecha` and `longbow` on `PATH`
- the bundled standard library, discovered automatically by Longbow
- Clang 18 as Flecha's native linker
- Neovim 0.12.6 with the Flecha plugin and the host configuration when present
- a starter Longbow program at `/workspace/hello`
- an unprivileged `flecha` user

Docker Engine must be installed and running on the host.

## Create the sandbox

From the repository root, run:

```sh
make sandbox
```

This builds the compiler and tools from the current working tree, including
uncommitted files that are not excluded by `.dockerignore`. It then replaces
the container named `flecha-sandbox` and starts the new container in the
background. The host Git description is embedded in both tools, so their
`--version` output identifies the checkout used to create the sandbox.

If the host has a Neovim configuration, the sandbox copies it from
`$XDG_CONFIG_HOME/nvim`, or from `$HOME/.config/nvim` when
`XDG_CONFIG_HOME` is unset. The copy is writable inside the disposable
container, so plugin managers can maintain their lockfiles without changing
the host configuration. Neovim's data, cache, and downloaded plugins also
remain inside the container. When the host directory does not exist, the
sandbox uses a minimal configuration that enables Flecha filetype detection
and syntax highlighting.

The repository is mounted read-only at its original absolute path. This lets a
host configuration that points directly at `editors/neovim` keep using the
current checkout. Compiler changes still require another `make sandbox`.

Set `HOST_NVIM_CONFIG` to use a different directory. Point it at a nonexistent
directory to force the minimal configuration:

```sh
make sandbox HOST_NVIM_CONFIG=/path/to/nvim
make sandbox HOST_NVIM_CONFIG=/nonexistent
```

Re-run `make sandbox` whenever the compiler or editor plugin changes. Files
created inside the old container are removed when it is replaced, so copy out
anything you want to keep first.

## Enter the sandbox

```sh
make enter-sandbox
```

This starts the container if necessary and opens an interactive login shell in
`/workspace`. Try the installed tools with:

```sh
flecha --version
longbow --version
cd /workspace/hello
longbow check
longbow run
nvim src/main.fl
```

The container remains available after leaving the shell. Stop or remove it
with normal Docker commands:

```sh
docker stop flecha-sandbox
docker rm -f flecha-sandbox
```

The image and container names can be changed without editing the Makefile:

```sh
make sandbox \
    SANDBOX_IMAGE=my-flecha:dev \
    SANDBOX_CONTAINER=my-flecha \
    HOST_NVIM_CONFIG=/path/to/nvim
make enter-sandbox SANDBOX_CONTAINER=my-flecha
```
