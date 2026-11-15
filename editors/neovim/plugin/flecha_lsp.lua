if vim.g.loaded_flecha_lsp then
    return
end
vim.g.loaded_flecha_lsp = true

local lsp = require("flecha.lsp")
if not lsp.is_configured() then
    local options = vim.deepcopy(vim.g.flecha_lsp or {})
    if vim.g.flecha_lsp_cmd ~= nil then
        options.cmd = vim.g.flecha_lsp_cmd
        options.server_path = vim.g.flecha_lsp_cmd[1]
    end
    if vim.g.flecha_compiler_path ~= nil then
        options.compiler_path = vim.g.flecha_compiler_path
    end
    if vim.g.flecha_lsp_autostart == 0 then
        options.enabled = false
    end
    lsp.setup(options)
end

vim.api.nvim_create_user_command("FlechaLspInfo", function()
    local info = lsp.info()
    vim.notify(vim.inspect(info), vim.log.levels.INFO, { title = "Flecha LSP" })
end, { desc = "Show the resolved Flecha language-server configuration" })
