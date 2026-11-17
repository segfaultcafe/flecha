local codex = require("flecha.codex")

local starts = {}
local sent = {}
local stopped = {}
local next_job = 70
local cache_directory = vim.fn.tempname()
local cache_path = cache_directory .. "/codex-chats.json"
local fake_transport = {
    start = function(command, options)
        next_job = next_job + 1
        table.insert(starts, {
            job = next_job,
            command = command,
            options = options,
        })
        return next_job
    end,
    send = function(job, text)
        table.insert(sent, { job = job, text = text })
    end,
    close_stdin = function() end,
    stop = function(job)
        table.insert(stopped, job)
    end,
}

codex.setup({
    command = { "missing-codex-test-command" },
    fallback_paths = { vim.v.progpath },
    width = 44,
    min_width = 32,
    max_width = 60,
    cache_path = cache_path,
    max_cached_chats = 3,
})
codex._set_transport(fake_transport)

local source_window = vim.api.nvim_get_current_win()
local source_buffer = vim.api.nvim_get_current_buf()
codex.open()

local state = codex._state
assert(vim.api.nvim_win_is_valid(state.transcript_window))
assert(vim.api.nvim_win_is_valid(state.input_window))
assert(vim.api.nvim_win_get_width(state.transcript_window) == 44)
assert(vim.api.nvim_buf_get_option(state.transcript_buffer, "filetype") == "markdown")
assert(vim.api.nvim_buf_get_option(state.input_buffer, "filetype") == "markdown")
assert(vim.api.nvim_get_current_win() == state.input_window)
assert(vim.fn.maparg("<C-S>", "n", false, true).buffer == 1)
assert(vim.fn.exists(":FlechaCodexChats") == 2)
assert(vim.fn.maparg("<leader>ml", "n") ~= "")
assert(codex.info().root == vim.fn.resolve(vim.fn.getcwd()))
assert(codex.info().root_kind == "git")
assert(codex.info().skip_git_repo_check == false)

local marker_directory = vim.fn.tempname()
vim.fn.mkdir(marker_directory .. "/src", "p")
vim.fn.writefile({ "[project]" }, marker_directory .. "/longbow.toml")
local marker_buffer = vim.api.nvim_create_buf(false, true)
vim.api.nvim_buf_set_name(marker_buffer, marker_directory .. "/src/example.fl")
local marker_root, marker_skip, marker_kind = codex._find_root(marker_buffer)
assert(marker_root == vim.fn.resolve(marker_directory))
assert(marker_skip == true)
assert(marker_kind == "longbow.toml")

local previous_root = state.root
local previous_kind = state.root_kind
local previous_skip = state.skip_git_repo_check
state.root = marker_root
state.root_kind = marker_kind
state.skip_git_repo_check = marker_skip
local marker_command = codex._command_for_turn({ "codex" })
assert(vim.tbl_contains(marker_command, "--skip-git-repo-check"))
state.root = previous_root
state.root_kind = previous_kind
state.skip_git_repo_check = previous_skip

local plain_directory = vim.fn.tempname()
vim.fn.mkdir(plain_directory, "p")
local plain_buffer = vim.api.nvim_create_buf(false, true)
vim.api.nvim_buf_set_name(plain_buffer, plain_directory .. "/example.fl")
local plain_root, plain_skip, plain_kind = codex._find_root(plain_buffer)
assert(plain_root == vim.fn.resolve(plain_directory))
assert(plain_skip == true)
assert(plain_kind == "file-directory")
vim.fn.delete(marker_directory, "rf")
vim.fn.delete(plain_directory, "rf")

vim.api.nvim_buf_set_lines(state.input_buffer, 0, -1, false, {
    "Explain the active function,",
    "then improve its name.",
})
codex.send()
assert(#starts == 1)
assert(starts[1].command[1] == vim.fn.fnamemodify(vim.v.progpath, ":p"))
assert(starts[1].command[2] == "exec")
assert(vim.tbl_contains(starts[1].command, "--json"))
assert(vim.tbl_contains(starts[1].command, "--approve-for-me"))
assert(not vim.tbl_contains(starts[1].command, "--sandbox"))
assert(not vim.tbl_contains(starts[1].command, "--skip-git-repo-check"))
assert(vim.tbl_contains(starts[1].command, "--cd"))
assert(starts[1].options.cwd == codex.info().root)
assert(#sent == 1)
assert(sent[1].text:find("Explain the active function", 1, true))
assert(sent[1].text:find("<editor_context>", 1, true))

local callbacks = starts[1].options
callbacks.on_stdout(71, {
    '{"type":"thread.started","thread_id":"thread-test-123"}',
    '{"type":"item.started","item":{"type":"command_execution"}}',
    '{"type":"item.completed","item":{"type":"command_execution","command":"rg active_function"}}',
    '{"type":"item.completed","item":{"type":"file_change","changes":[{"path":"src/example.cpp"}]}}',
    '{"type":"item.completed","item":{"type":"agent_message","text":"Updated the function name."}}',
    '{"type":"turn.completed"}',
    "",
})
callbacks.on_exit(71, 0)
vim.wait(1000, function()
    return codex.info().running == false
end)

assert(codex.info().thread_id == "thread-test-123")
assert(codex.info().status:find("Ready", 1, true))
local transcript = table.concat(
    vim.api.nvim_buf_get_lines(state.transcript_buffer, 0, -1, false),
    "\n"
)
assert(transcript:find("## You", 1, true))
assert(transcript:find("## Codex", 1, true))
assert(transcript:find("Updated the function name.", 1, true))
assert(transcript:find("src/example.cpp", 1, true))

assert(vim.fn.filereadable(cache_path) == 1)
local decode = vim.json and vim.json.decode or vim.fn.json_decode
local cache = decode(table.concat(vim.fn.readfile(cache_path), "\n"))
local cached_chats = cache.roots[codex.info().root]
assert(#cached_chats == 1)
assert(cached_chats[1].id == "thread-test-123")
assert(cached_chats[1].title == "Explain the active function, then improve its name.")
assert(#cached_chats[1].messages == 2)

codex.new_chat()
assert(codex.info().thread_id == nil)
local original_select = vim.ui.select
local selection_count = 0
vim.ui.select = function(items, selection_options, on_choice)
    selection_count = selection_count + 1
    assert(#items == 1)
    assert(selection_options.prompt:find("Codex chats", 1, true))
    assert(selection_options.format_item(items[1]):find(items[1].title, 1, true))
    on_choice(items[1], 1)
end
codex.chats()
vim.ui.select = original_select
assert(selection_count == 1)
assert(codex.info().thread_id == "thread-test-123")
transcript = table.concat(
    vim.api.nvim_buf_get_lines(state.transcript_buffer, 0, -1, false),
    "\n"
)
assert(transcript:find("Explain the active function", 1, true))
assert(transcript:find("Updated the function name.", 1, true))

vim.api.nvim_buf_set_lines(state.input_buffer, 0, -1, false, { "Now add a test." })
codex.send()
assert(#starts == 2)
assert(starts[2].command[2] == "exec")
assert(vim.tbl_contains(starts[2].command, "resume"))
assert(vim.tbl_contains(starts[2].command, "--approve-for-me"))
assert(not vim.tbl_contains(starts[2].command, "--sandbox"))
assert(not vim.tbl_contains(starts[2].command, "--skip-git-repo-check"))
assert(vim.tbl_contains(starts[2].command, "thread-test-123"))
codex.stop()
assert(stopped[#stopped] == 72)
assert(codex.info().running == false)

codex.new_chat()
assert(codex.info().thread_id == nil)
codex.close()
assert(state.transcript_window == nil)
assert(state.input_window == nil)
assert(vim.api.nvim_get_current_win() == source_window)
assert(vim.api.nvim_get_current_buf() == source_buffer)
vim.fn.delete(cache_directory, "rf")
