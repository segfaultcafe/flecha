local test_directory = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h")
local fixture_directory = test_directory .. "/fixtures"
local lsp = require("flecha.lsp")

local function executable(path)
    vim.fn.mkdir(vim.fn.fnamemodify(path, ":h"), "p")
    vim.fn.writefile({ "#!/bin/sh", "exit 0" }, path)
    assert(vim.fn.setfperm(path, "rwxr-xr-x") == 1)
end

-- A relocated release is accepted only as a complete server/compiler pair.
local temporary = vim.fn.tempname()
local plugin_root = temporary .. "/share/flecha/editors/neovim"
local server = temporary .. "/bin/flecha-lsp"
local compiler = temporary .. "/bin/flecha"
vim.fn.mkdir(plugin_root, "p")
executable(server)
executable(compiler)

local resolved_server, resolved_compiler = lsp._resolve_tools({}, plugin_root)
assert(resolved_server == vim.fn.fnamemodify(server, ":p"))
assert(resolved_compiler == vim.fn.fnamemodify(compiler, ":p"))

vim.fn.delete(compiler)
resolved_server = lsp._resolve_tools({}, plugin_root)
assert(resolved_server ~= vim.fn.fnamemodify(server, ":p"))
executable(compiler)

-- Prefer a complete compiler installation over an obsolete standalone server
-- that appears earlier on PATH.
local old_bin = temporary .. "/old/bin"
local current_bin = temporary .. "/current/bin"
executable(old_bin .. "/flecha-lsp")
executable(current_bin .. "/flecha")
executable(current_bin .. "/flecha-lsp")
local original_path = vim.env.PATH
vim.env.PATH = old_bin .. ":" .. current_bin .. ":" .. original_path
resolved_server, resolved_compiler = lsp._resolve_tools(
    {},
    temporary .. "/standalone-plugin"
)
assert(resolved_server == vim.fn.fnamemodify(current_bin .. "/flecha-lsp", ":p"))
assert(resolved_compiler == vim.fn.fnamemodify(current_bin .. "/flecha", ":p"))
vim.env.PATH = original_path

-- The nearest longbow.toml wins over an outer .git marker.
local outer = temporary .. "/workspace"
local project = outer .. "/nested/project"
vim.fn.mkdir(outer .. "/.git", "p")
vim.fn.mkdir(project .. "/src", "p")
vim.fn.writefile({}, project .. "/longbow.toml")
vim.fn.writefile({ "i32 main() { return 0; }" }, project .. "/src/main.fl")
local root_buffer = vim.fn.bufadd(project .. "/src/main.fl")
assert(lsp._find_root(root_buffer, { "longbow.toml", ".git" }) == vim.fn.resolve(project))
local markerless = temporary .. "/markerless/src"
vim.fn.mkdir(markerless, "p")
vim.fn.writefile({}, markerless .. "/main.fl")
local markerless_buffer = vim.fn.bufadd(markerless .. "/main.fl")
local markerless_root = lsp._find_root(markerless_buffer, { "marker-that-does-not-exist" })
assert(markerless_root == vim.fn.resolve(markerless), markerless_root .. " != " .. vim.fn.resolve(markerless))

-- Stub Neovim's process-facing API: setup must pass the compiler through the
-- environment and reuse an existing Flecha client for the same workspace.
local original_start = vim.lsp.start
local original_get_clients = vim.lsp.get_clients
local original_get_active_clients = vim.lsp.get_active_clients
local original_is_attached = vim.lsp.buf_is_attached
local original_attach = vim.lsp.buf_attach_client
local starts = {}
local active = {}
local attachments = {}

vim.lsp.start = function(config, opts)
    table.insert(starts, { config = config, opts = opts })
    return 41
end
vim.lsp.get_clients = function()
    return active
end
vim.lsp.get_active_clients = vim.lsp.get_clients
vim.lsp.buf_is_attached = function()
    return false
end
vim.lsp.buf_attach_client = function(bufnr, client_id)
    table.insert(attachments, { bufnr, client_id })
    return true
end

local current = vim.api.nvim_get_current_buf()
assert(vim.bo[current].filetype == "flecha")
lsp.setup({
    cmd = { server, "--test-option" },
    compiler_path = compiler,
})
assert(#starts == 1)
assert(starts[1].config.cmd[1] == vim.fn.fnamemodify(server, ":p"))
assert(starts[1].config.cmd[2] == "--test-option")
assert(starts[1].config.cmd_env.FLECHA_COMPILER == vim.fn.fnamemodify(compiler, ":p"))
assert(starts[1].config.root_dir == vim.fn.resolve(fixture_directory))

active = {
    {
        id = 73,
        name = "configured-elsewhere",
        config = {
            cmd = { server },
            root_dir = starts[1].config.root_dir,
        },
    },
}
-- Neovim 0.8 exposes get_active_clients but not get_clients.
vim.lsp.get_clients = nil
vim.api.nvim_exec_autocmds("FileType", { buffer = current, modeline = false })
assert(#starts == 1)
assert(#attachments == 1)
assert(attachments[1][1] == current and attachments[1][2] == 73)

-- An externally started client is reusable even when the configured tools are
-- absent; discovery must not reject it or emit a misleading missing-tool warning.
lsp.setup({ cmd = { temporary .. "/missing/flecha-lsp" } })
assert(#starts == 1)
assert(#attachments == 2)

-- Keep syntax/indent support usable on old Neovim versions without LSP APIs.
vim.lsp.start = nil
vim.api.nvim_exec_autocmds("FileType", { buffer = current, modeline = false })
assert(#starts == 1)

lsp.setup({ enabled = false })
vim.lsp.start = original_start
vim.lsp.get_clients = original_get_clients
vim.lsp.get_active_clients = original_get_active_clients
vim.lsp.buf_is_attached = original_is_attached
vim.lsp.buf_attach_client = original_attach
vim.fn.delete(temporary, "rf")

local source = table.concat(vim.fn.readfile(
    vim.fn.fnamemodify(test_directory .. "/../lua/flecha/lsp.lua", ":p")
), "\n")
assert(not source:find("vim%.fs%."), "Neovim 0.8 compatibility forbids vim.fs APIs")
