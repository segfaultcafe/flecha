if vim.g.loaded_flecha_codex then
    return
end
vim.g.loaded_flecha_codex = true

local codex = require("flecha.codex")
if not codex.is_configured() then
    codex.setup(vim.deepcopy(vim.g.flecha_codex or {}))
end

vim.api.nvim_create_user_command("FlechaCodex", codex.toggle, {
    desc = "Toggle the Codex project chat",
})
vim.api.nvim_create_user_command("FlechaCodexNew", codex.new_chat, {
    desc = "Start a new Codex project chat",
})
vim.api.nvim_create_user_command("FlechaCodexStop", codex.stop, {
    desc = "Stop the active Codex turn",
})
vim.api.nvim_create_user_command("FlechaCodexChats", codex.chats, {
    desc = "List cached Codex chats for this project",
})
vim.api.nvim_create_user_command("FlechaCodexInfo", function()
    vim.notify(vim.inspect(codex.info()), vim.log.levels.INFO, { title = "Flecha Codex" })
end, {
    desc = "Show the Codex project chat configuration",
})

vim.keymap.set("n", "<Plug>(FlechaCodexToggle)", codex.toggle, {
    silent = true,
    desc = "Toggle the Codex project chat",
})
if vim.fn.hasmapto("<Plug>(FlechaCodexToggle)", "n") == 0
    and vim.fn.maparg("<leader>mc", "n") == ""
then
    vim.keymap.set("n", "<leader>mc", "<Plug>(FlechaCodexToggle)", {
        remap = true,
        silent = true,
        desc = "Toggle the Codex project chat",
    })
end

vim.keymap.set("n", "<Plug>(FlechaCodexChats)", codex.chats, {
    silent = true,
    desc = "List cached Codex chats for this project",
})
if vim.fn.hasmapto("<Plug>(FlechaCodexChats)", "n") == 0
    and vim.fn.maparg("<leader>ml", "n") == ""
then
    vim.keymap.set("n", "<leader>ml", "<Plug>(FlechaCodexChats)", {
        remap = true,
        silent = true,
        desc = "List cached Codex chats for this project",
    })
end
