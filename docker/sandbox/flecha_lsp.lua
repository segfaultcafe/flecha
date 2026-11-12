-- Host Neovim configurations may point at a Flecha build whose shared
-- libraries only exist on the host. Keep the disposable sandbox on the
-- compiler and language server installed together in /opt/flecha.
local server = vim.env.FLECHA_LSP or "/opt/flecha/bin/flecha-lsp"
local compiler = vim.env.FLECHA_COMPILER or "/opt/flecha/bin/flecha"

if not server or vim.fn.executable(server) ~= 1
    or not compiler or vim.fn.executable(compiler) ~= 1
then
    return
end

vim.g.flecha_lsp_cmd = { server }
vim.g.flecha_compiler_path = compiler

local native_enabled = vim.lsp
    and type(vim.lsp.is_enabled) == "function"
    and vim.lsp.is_enabled("flecha")

if native_enabled then
    vim.lsp.config("flecha", {
        cmd = { server },
        cmd_env = { FLECHA_COMPILER = compiler },
    })
    return
end

local loaded, lsp = pcall(require, "flecha.lsp")
if loaded then
    lsp.setup({
        server_path = server,
        compiler_path = compiler,
    })
end
