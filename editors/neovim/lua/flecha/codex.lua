local M = {}

local api = vim.api
local uv = vim.uv or vim.loop
local namespace = api.nvim_create_namespace("FlechaCodex")

local defaults = {
    command = { "codex" },
    fallback_paths = {},
    width = 0.38,
    min_width = 38,
    max_width = 72,
    input_height = 6,
    root_markers = { ".git", "longbow.toml" },
    cache_path = vim.fn.stdpath("data") .. "/flecha/codex-chats.json",
    max_cached_chats = 50,
}

local options = vim.deepcopy(defaults)
local configured = false
local state = {
    transcript_buffer = nil,
    input_buffer = nil,
    transcript_window = nil,
    input_window = nil,
    source_window = nil,
    source_buffer = nil,
    root = nil,
    root_kind = nil,
    skip_git_repo_check = false,
    thread_id = nil,
    messages = {},
    chat_title = nil,
    chat_created_at = nil,
    job = nil,
    stdout = "",
    stderr = "",
    stderr_pending = "",
    error_reported = false,
    spinner = nil,
    spinner_index = 1,
    status = "Ready",
}

local transport = {
    start = function(command, job_options)
        return vim.fn.jobstart(command, job_options)
    end,
    send = function(job, text)
        return vim.fn.chansend(job, text)
    end,
    close_stdin = function(job)
        return vim.fn.chanclose(job, "stdin")
    end,
    stop = function(job)
        return vim.fn.jobstop(job)
    end,
}

local function valid_buffer(buffer)
    return buffer and api.nvim_buf_is_valid(buffer)
end

local function valid_window(window)
    return window and api.nvim_win_is_valid(window)
end

local function normalize(path)
    if not path or path == "" then
        return nil
    end
    local full = vim.fn.fnamemodify(vim.fn.expand(path), ":p")
    if full == "/" or full:match("^%a:[/\\]$") then
        return full
    end
    return full:gsub("[/\\]+$", "")
end

local function dirname(path)
    return vim.fn.fnamemodify(path, ":h")
end

local function join(directory, name)
    return directory:gsub("[/\\]+$", "") .. "/" .. name
end

local function canonical(path)
    path = normalize(path)
    return path and (uv.fs_realpath(path) or path) or nil
end

local function json_decode(text)
    local decoder = vim.json and vim.json.decode or vim.fn.json_decode
    local ok, value = pcall(decoder, text)
    return ok and value or nil
end

local function json_encode(value)
    local encoder = vim.json and vim.json.encode or vim.fn.json_encode
    local ok, encoded = pcall(encoder, value)
    return ok and encoded or nil
end

local function empty_cache()
    return { version = 1, roots = {} }
end

local function read_cache()
    local path = normalize(options.cache_path)
    if not path or vim.fn.filereadable(path) ~= 1 then
        return empty_cache()
    end
    local ok, lines = pcall(vim.fn.readfile, path)
    if not ok then
        return empty_cache()
    end
    local cache = json_decode(table.concat(lines, "\n"))
    if type(cache) ~= "table" or type(cache.roots) ~= "table" then
        return empty_cache()
    end
    return cache
end

local function write_cache(cache)
    local path = normalize(options.cache_path)
    local encoded = json_encode(cache)
    if not path or not encoded then
        return false
    end
    vim.fn.mkdir(dirname(path), "p")
    local ok, result = pcall(vim.fn.writefile, { encoded }, path)
    return ok and result == 0
end

local function title_from_messages(messages)
    for _, message in ipairs(messages) do
        if message.role == "user" and type(message.text) == "string" then
            local title = message.text:gsub("%s+", " "):match("^%s*(.-)%s*$")
            if #title > 56 then
                title = title:sub(1, 53) .. "..."
            end
            return title ~= "" and title or "Untitled chat"
        end
    end
    return "Untitled chat"
end

local function save_current_chat()
    if not state.root or not state.thread_id then
        return
    end
    local cache = read_cache()
    local root = canonical(state.root)
    local chats = cache.roots[root]
    if type(chats) ~= "table" then
        chats = {}
    end

    local existing
    for index, chat in ipairs(chats) do
        if chat.id == state.thread_id then
            existing = table.remove(chats, index)
            break
        end
    end
    local now = os.time()
    state.chat_title = state.chat_title or title_from_messages(state.messages)
    state.chat_created_at = state.chat_created_at
        or (existing and existing.created_at)
        or now
    table.insert(chats, 1, {
        id = state.thread_id,
        title = state.chat_title,
        created_at = state.chat_created_at,
        updated_at = now,
        messages = vim.deepcopy(state.messages),
    })
    local limit = math.max(1, tonumber(options.max_cached_chats) or 50)
    while #chats > limit do
        table.remove(chats)
    end
    cache.roots[root] = chats
    write_cache(cache)
end

local function chats_for_root(root)
    local chats = read_cache().roots[canonical(root)] or {}
    table.sort(chats, function(left, right)
        return (left.updated_at or 0) > (right.updated_at or 0)
    end)
    return chats
end

local function executable_name()
    return vim.fn.has("win32") == 1 and "codex.exe" or "codex"
end

local function default_codex_paths()
    local executable = executable_name()
    local paths = {}
    if vim.env.CODEX_INSTALL_DIR and vim.env.CODEX_INSTALL_DIR ~= "" then
        table.insert(paths, join(vim.env.CODEX_INSTALL_DIR, executable))
    end
    if vim.fn.has("win32") == 1 then
        if vim.env.LOCALAPPDATA and vim.env.LOCALAPPDATA ~= "" then
            table.insert(paths, join(
                vim.env.LOCALAPPDATA,
                "Programs/OpenAI/Codex/bin/" .. executable
            ))
        end
    else
        table.insert(paths, vim.fn.expand("~/.local/bin/" .. executable))
        table.insert(paths, vim.fn.expand("~/bin/" .. executable))
        table.insert(paths, "/usr/lib/chatgpt/resources/" .. executable)
        table.insert(paths, "/opt/homebrew/bin/" .. executable)
        table.insert(paths, "/usr/local/bin/" .. executable)
        table.insert(paths, "/usr/bin/" .. executable)
    end
    return paths
end

local function resolve_command()
    local command = vim.deepcopy(options.command)
    local configured_command = command[1]
    local resolved = vim.fn.exepath(configured_command)
    if resolved ~= "" then
        command[1] = normalize(resolved)
        return command
    end

    if configured_command:find("[/\\]") then
        local path = normalize(configured_command)
        if path and vim.fn.executable(path) == 1 then
            command[1] = path
            return command
        end
    end

    local candidates = vim.deepcopy(options.fallback_paths or {})
    vim.list_extend(candidates, default_codex_paths())
    for _, candidate in ipairs(candidates) do
        local path = normalize(candidate)
        if path and vim.fn.executable(path) == 1 then
            command[1] = path
            return command
        end
    end
    return nil
end

local function git_root(directory)
    if not directory or vim.fn.executable("git") ~= 1 then
        return nil
    end
    local output = vim.fn.systemlist({
        "git",
        "-C",
        directory,
        "rev-parse",
        "--show-toplevel",
    })
    if vim.v.shell_error ~= 0 or not output[1] or output[1] == "" then
        return nil
    end
    return canonical(output[1])
end

local function find_root(buffer)
    local name = valid_buffer(buffer) and api.nvim_buf_get_name(buffer) or ""
    local directory = name ~= "" and dirname(normalize(name))
        or canonical(vim.fn.getcwd())
    local source_directory = directory

    local repository = git_root(directory)
    if repository then
        return repository, false, "git"
    end

    while directory do
        for _, marker in ipairs(options.root_markers) do
            if marker ~= ".git" and uv.fs_stat(join(directory, marker)) then
                return canonical(directory), true, marker
            end
        end
        local parent = dirname(directory)
        if parent == directory then
            break
        end
        directory = parent
    end

    if name ~= "" then
        return canonical(source_directory), true, "file-directory"
    end
    return nil, false, nil
end

local function set_buffer_option(buffer, name, value)
    api.nvim_buf_set_option(buffer, name, value)
end

local function set_window_option(window, name, value)
    api.nvim_win_set_option(window, name, value)
end

local function set_modifiable(buffer, value)
    if valid_buffer(buffer) then
        set_buffer_option(buffer, "modifiable", value)
    end
end

local function split_lines(text)
    if text == "" then
        return { "" }
    end
    return vim.split(text, "\n", { plain = true })
end

local function should_follow()
    if not valid_window(state.transcript_window)
        or not valid_buffer(state.transcript_buffer)
    then
        return false
    end
    local cursor = api.nvim_win_get_cursor(state.transcript_window)[1]
    local count = api.nvim_buf_line_count(state.transcript_buffer)
    return cursor >= count - 2
end

local function scroll_to_bottom(force)
    if not force
        or not valid_window(state.transcript_window)
        or not valid_buffer(state.transcript_buffer)
    then
        return
    end
    local count = api.nvim_buf_line_count(state.transcript_buffer)
    api.nvim_win_set_cursor(state.transcript_window, { count, 0 })
end

local function append_lines(lines)
    if not valid_buffer(state.transcript_buffer) then
        return
    end
    local follow = should_follow()
    set_modifiable(state.transcript_buffer, true)
    api.nvim_buf_set_lines(state.transcript_buffer, -1, -1, false, lines)
    set_modifiable(state.transcript_buffer, false)
    scroll_to_bottom(follow)
end

local function append_message(role, text, record)
    local heading = role == "user" and "## You" or "## Codex"
    local lines = { "", heading, "" }
    vim.list_extend(lines, split_lines(text))
    append_lines(lines)
    if record ~= false then
        table.insert(state.messages, { role = role, text = text })
        save_current_chat()
    end
end

local function append_activity(icon, text)
    text = tostring(text or ""):gsub("%s+", " ")
    if #text > 140 then
        text = text:sub(1, 137) .. "..."
    end
    append_lines({ "", "> " .. icon .. " " .. text })
end

local function escape_status(text)
    return tostring(text):gsub("%%", "%%%%")
end

local function render_status(text)
    state.status = text
    if valid_window(state.transcript_window) then
        local project = state.root and vim.fn.fnamemodify(state.root, ":t") or "project"
        local bar = "%#FlechaCodexTitle#  CODEX  "
            .. "%#FlechaCodexMuted#"
            .. escape_status(project)
            .. "  %#FlechaCodexStatus#"
            .. escape_status(text)
            .. " "
        set_window_option(state.transcript_window, "winbar", bar)
    end
end

local function stop_spinner()
    if state.spinner then
        state.spinner:stop()
        state.spinner:close()
        state.spinner = nil
    end
end

local function start_spinner(label)
    stop_spinner()
    state.spinner_index = 1
    local frames = { "◐", "◓", "◑", "◒" }
    state.spinner = uv.new_timer()
    state.spinner:start(0, 120, vim.schedule_wrap(function()
        if not state.job then
            return
        end
        render_status(frames[state.spinner_index] .. " " .. label)
        state.spinner_index = state.spinner_index % #frames + 1
    end))
end

local function refresh_placeholder()
    if not valid_buffer(state.input_buffer) then
        return
    end
    api.nvim_buf_clear_namespace(state.input_buffer, namespace, 0, -1)
    local lines = api.nvim_buf_get_lines(state.input_buffer, 0, -1, false)
    if #lines == 1 and lines[1] == "" then
        api.nvim_buf_set_extmark(state.input_buffer, namespace, 0, 0, {
            virt_text = { { "Ask Codex to inspect or edit this project…", "Comment" } },
            virt_text_pos = "overlay",
            hl_mode = "combine",
        })
    end
end

local function create_buffer(name, writable)
    local buffer = api.nvim_create_buf(false, true)
    pcall(api.nvim_buf_set_name, buffer, name)
    set_buffer_option(buffer, "buftype", "nofile")
    set_buffer_option(buffer, "bufhidden", "hide")
    set_buffer_option(buffer, "swapfile", false)
    set_buffer_option(buffer, "undofile", false)
    set_buffer_option(buffer, "filetype", "markdown")
    set_buffer_option(buffer, "modifiable", writable)
    return buffer
end

local function ensure_buffers()
    if not valid_buffer(state.transcript_buffer) then
        state.transcript_buffer = create_buffer("codex://flecha/conversation", false)
    end
    if not valid_buffer(state.input_buffer) then
        state.input_buffer = create_buffer("codex://flecha/prompt", true)
        api.nvim_buf_set_lines(state.input_buffer, 0, -1, false, { "" })
        local group = api.nvim_create_augroup("FlechaCodexInput", { clear = true })
        api.nvim_create_autocmd({ "TextChanged", "TextChangedI" }, {
            group = group,
            buffer = state.input_buffer,
            callback = refresh_placeholder,
            desc = "Update the Codex prompt placeholder",
        })
    end
end

local function render_welcome()
    if not valid_buffer(state.transcript_buffer) then
        return
    end
    local root = state.root and vim.fn.fnamemodify(state.root, ":~")
        or "No Git or project-marker root detected"
    local lines = {
        "# Codex",
        "",
        "`" .. root .. "`",
        "",
        "Chat with the agent working in this project. It can inspect files,",
        "run commands, and make edits inside the workspace.",
        "",
        "---",
    }
    set_modifiable(state.transcript_buffer, true)
    api.nvim_buf_set_lines(state.transcript_buffer, 0, -1, false, lines)
    set_modifiable(state.transcript_buffer, false)
end

local function render_conversation()
    render_welcome()
    for _, message in ipairs(state.messages) do
        append_message(message.role, message.text, false)
    end
    scroll_to_bottom(true)
end

local function panel_width()
    local requested = options.width
    if requested > 0 and requested < 1 then
        requested = math.floor(vim.o.columns * requested)
    end
    requested = math.floor(requested)
    local available = math.max(20, vim.o.columns - 24)
    return math.min(math.max(options.min_width, requested), options.max_width, available)
end

local function style_window(window, input)
    set_window_option(window, "number", false)
    set_window_option(window, "relativenumber", false)
    set_window_option(window, "signcolumn", "no")
    set_window_option(window, "foldcolumn", "0")
    set_window_option(window, "wrap", true)
    set_window_option(window, "linebreak", true)
    set_window_option(window, "cursorline", input)
    set_window_option(window, "winfixwidth", true)
    set_window_option(window, "winhl", "Normal:FlechaCodexNormal,NormalNC:FlechaCodexNormal,EndOfBuffer:FlechaCodexNormal")
end

local function close_panel()
    local return_window = state.source_window
    if valid_window(state.input_window) then
        api.nvim_win_close(state.input_window, true)
    end
    if valid_window(state.transcript_window) then
        api.nvim_win_close(state.transcript_window, true)
    end
    state.input_window = nil
    state.transcript_window = nil
    if valid_window(return_window) then
        api.nvim_set_current_win(return_window)
    end
end

local function focus_input()
    if valid_window(state.input_window) then
        api.nvim_set_current_win(state.input_window)
        vim.cmd("startinsert")
    end
end

local function install_buffer_mappings()
    local common = { silent = true, nowait = true }
    for _, buffer in ipairs({ state.transcript_buffer, state.input_buffer }) do
        vim.keymap.set("n", "q", close_panel, vim.tbl_extend("force", common, { buffer = buffer }))
        vim.keymap.set("n", "<leader>mc", close_panel, vim.tbl_extend("force", common, { buffer = buffer }))
    end
    vim.keymap.set("n", "i", focus_input, {
        buffer = state.transcript_buffer,
        silent = true,
        desc = "Focus the Codex prompt",
    })
    vim.keymap.set("n", "a", focus_input, {
        buffer = state.transcript_buffer,
        silent = true,
        desc = "Focus the Codex prompt",
    })
    vim.keymap.set({ "n", "i" }, "<C-s>", function()
        M.send()
    end, {
        buffer = state.input_buffer,
        silent = true,
        desc = "Send the prompt to Codex",
    })
    vim.keymap.set("n", "<CR>", function()
        M.send()
    end, {
        buffer = state.input_buffer,
        silent = true,
        desc = "Send the prompt to Codex",
    })
    vim.keymap.set("n", "<C-c>", function()
        M.stop()
    end, {
        buffer = state.input_buffer,
        silent = true,
        desc = "Stop the active Codex turn",
    })
    vim.keymap.set("n", "r", function()
        M.new_chat()
    end, {
        buffer = state.transcript_buffer,
        silent = true,
        desc = "Start a new Codex chat",
    })
    vim.keymap.set("n", "l", function()
        M.chats()
    end, {
        buffer = state.transcript_buffer,
        silent = true,
        desc = "List cached Codex chats",
    })
end

local function open_panel()
    state.source_window = api.nvim_get_current_win()
    state.source_buffer = api.nvim_get_current_buf()
    local root, skip_git_repo_check, root_kind = find_root(state.source_buffer)
    if (state.root or root)
        and (canonical(state.root) ~= canonical(root)
            or state.skip_git_repo_check ~= skip_git_repo_check)
    then
        state.thread_id = nil
        state.messages = {}
        state.chat_title = nil
        state.chat_created_at = nil
        state.transcript_buffer = nil
    end
    state.root = root
    state.root_kind = root_kind
    state.skip_git_repo_check = skip_git_repo_check

    ensure_buffers()
    if api.nvim_buf_line_count(state.transcript_buffer) == 1
        and api.nvim_buf_get_lines(state.transcript_buffer, 0, 1, false)[1] == ""
    then
        render_welcome()
    end

    vim.cmd("botright vsplit")
    state.transcript_window = api.nvim_get_current_win()
    api.nvim_win_set_buf(state.transcript_window, state.transcript_buffer)
    api.nvim_win_set_width(state.transcript_window, panel_width())
    style_window(state.transcript_window, false)

    vim.cmd("belowright " .. tostring(options.input_height) .. "split")
    state.input_window = api.nvim_get_current_win()
    api.nvim_win_set_buf(state.input_window, state.input_buffer)
    api.nvim_win_set_height(state.input_window, options.input_height)
    style_window(state.input_window, true)
    set_window_option(
        state.input_window,
        "winbar",
        "%#FlechaCodexPrompt#  PROMPT  %#FlechaCodexMuted#Ctrl-S send  ·  <leader>ml chats  ·  Esc then q close "
    )

    install_buffer_mappings()
    render_status(state.job and "Working…"
        or (state.root and "Ready" or "No project root"))
    refresh_placeholder()
    scroll_to_bottom(true)
    focus_input()
end

local function changed_paths(item)
    local paths = {}
    if type(item.changes) == "table" then
        for _, change in ipairs(item.changes) do
            local path = type(change) == "table" and (change.path or change.file_path) or change
            if path then
                table.insert(paths, path)
            end
        end
    elseif item.path or item.file_path then
        table.insert(paths, item.path or item.file_path)
    end
    return #paths > 0 and table.concat(paths, ", ") or "workspace files"
end

local function handle_event(event)
    if type(event) ~= "table" then
        return
    end
    if event.type == "thread.started" and event.thread_id then
        state.thread_id = event.thread_id
        save_current_chat()
    elseif event.type == "item.started" and event.item then
        if event.item.type == "command_execution" then
            render_status("◐ Running command…")
        elseif event.item.type == "file_change" then
            render_status("◐ Editing files…")
        end
    elseif event.type == "item.completed" and event.item then
        local item = event.item
        if item.type == "agent_message" and item.text then
            append_message("assistant", item.text)
        elseif item.type == "command_execution" and item.command then
            append_activity("⟳", item.command)
        elseif item.type == "file_change" then
            append_activity("✎", "Edited " .. changed_paths(item))
        end
    elseif event.type == "turn.failed" or event.type == "error" then
        local message = event.message
            or (event.error and (event.error.message or event.error))
            or "The Codex turn failed."
        state.error_reported = true
        append_message("assistant", "**Error:** " .. tostring(message))
    elseif event.type == "turn.completed" then
        render_status("✓ Finishing…")
    end
end

local function consume_stream(kind, data)
    if not data or #data == 0 then
        return
    end
    local field = kind == "stdout" and "stdout" or "stderr_pending"
    data[1] = state[field] .. (data[1] or "")
    for index = 1, #data - 1 do
        local line = data[index]
        if kind == "stdout" and line ~= "" then
            local event = json_decode(line)
            if event then
                handle_event(event)
            end
        elseif kind == "stderr" and line ~= "" then
            state.stderr = state.stderr .. line .. "\n"
        end
    end
    state[field] = data[#data] or ""
end

local function finish_job(job, exit_code)
    if state.job ~= job then
        return
    end
    if state.stdout ~= "" then
        local event = json_decode(state.stdout)
        if event then
            handle_event(event)
        end
    end
    if state.stderr_pending ~= "" then
        state.stderr = state.stderr .. state.stderr_pending .. "\n"
        state.stderr_pending = ""
    end
    stop_spinner()
    state.job = nil
    if exit_code == 0 then
        render_status("✓ Ready")
        pcall(vim.cmd, "silent! checktime")
    else
        render_status("✕ Failed")
        if not state.error_reported then
            local message = state.stderr:gsub("%s+$", "")
            if message == "" then
                message = "Codex exited with status " .. tostring(exit_code) .. "."
            end
            append_message("assistant", "**Error:** " .. message)
        end
    end
end

local function editor_context()
    if not valid_buffer(state.source_buffer) then
        return ""
    end
    local name = api.nvim_buf_get_name(state.source_buffer)
    if name == "" then
        return ""
    end
    local absolute = normalize(name)
    local prefix = state.root and (state.root .. "/") or ""
    local relative = absolute
    if prefix ~= "" and absolute:sub(1, #prefix) == prefix then
        relative = absolute:sub(#prefix + 1)
    end
    local line = 1
    local column = 1
    if valid_window(state.source_window) then
        local cursor = api.nvim_win_get_cursor(state.source_window)
        line = cursor[1]
        column = cursor[2] + 1
    end
    return string.format(
        "\n\n<editor_context>\nactive_file: %s\ncursor: %d:%d\n</editor_context>",
        relative,
        line,
        column
    )
end

local function save_source_buffer()
    if not valid_buffer(state.source_buffer)
        or not api.nvim_buf_get_option(state.source_buffer, "modified")
        or api.nvim_buf_get_option(state.source_buffer, "buftype") ~= ""
    then
        return true
    end
    local ok, error_message = pcall(api.nvim_buf_call, state.source_buffer, function()
        vim.cmd("silent update")
    end)
    if not ok then
        vim.notify("Codex: could not save the active buffer: " .. error_message, vim.log.levels.ERROR)
    end
    return ok
end

local function command_for_turn(command)
    vim.list_extend(command, {
        "exec",
        "--json",
        "--approve-for-me",
    })
    if state.skip_git_repo_check then
        table.insert(command, "--skip-git-repo-check")
    end
    if state.thread_id then
        vim.list_extend(command, {
            "resume",
            state.thread_id,
            "-",
        })
    else
        vim.list_extend(command, {
            "--cd",
            state.root,
            "-",
        })
    end
    return command
end

function M.send()
    if state.job then
        vim.notify("Codex is still working. Press Ctrl-C in normal mode to stop it.", vim.log.levels.WARN)
        return
    end
    if not valid_buffer(state.input_buffer) then
        return
    end
    if not state.root then
        vim.notify(
            "Codex: no Git or project-marker root found. Open a project file and try again.",
            vim.log.levels.ERROR
        )
        return
    end
    local lines = api.nvim_buf_get_lines(state.input_buffer, 0, -1, false)
    local prompt = table.concat(lines, "\n"):match("^%s*(.-)%s*$")
    if not prompt or prompt == "" then
        vim.notify("Codex: type a prompt first.", vim.log.levels.INFO)
        return
    end
    local base_command = resolve_command()
    if not base_command then
        vim.notify(
            "Codex executable not found. Set vim.g.flecha_codex.command to its absolute path.",
            vim.log.levels.ERROR
        )
        return
    end
    if not save_source_buffer() then
        return
    end

    append_message("user", prompt)
    api.nvim_buf_set_lines(state.input_buffer, 0, -1, false, { "" })
    refresh_placeholder()
    state.stdout = ""
    state.stderr = ""
    state.stderr_pending = ""
    state.error_reported = false

    local command = command_for_turn(base_command)
    local job
    job = transport.start(command, {
        cwd = state.root,
        stdin = "pipe",
        stdout_buffered = false,
        stderr_buffered = false,
        on_stdout = vim.schedule_wrap(function(_, data)
            consume_stream("stdout", data)
        end),
        on_stderr = vim.schedule_wrap(function(_, data)
            consume_stream("stderr", data)
        end),
        on_exit = vim.schedule_wrap(function(_, exit_code)
            finish_job(job, exit_code)
        end),
    })
    if not job or job <= 0 then
        append_message("assistant", "**Error:** Codex could not be started.")
        render_status("✕ Failed")
        return
    end
    state.job = job
    start_spinner("Thinking…")
    transport.send(job, prompt .. editor_context())
    transport.close_stdin(job)
end

function M.open()
    if valid_window(state.transcript_window) or valid_window(state.input_window) then
        focus_input()
        return
    end
    open_panel()
end

function M.close()
    close_panel()
end

function M.toggle()
    if valid_window(state.transcript_window) or valid_window(state.input_window) then
        close_panel()
    else
        open_panel()
    end
end

function M.stop()
    if not state.job then
        vim.notify("Codex: there is no active turn.", vim.log.levels.INFO)
        return
    end
    local job = state.job
    transport.stop(job)
    stop_spinner()
    state.job = nil
    render_status("Stopped")
    append_activity("■", "Turn stopped")
end

function M.chats()
    if state.job then
        vim.notify("Codex: wait for the active turn to finish before switching chats.", vim.log.levels.WARN)
        return
    end
    local panel_open = valid_window(state.transcript_window)
        or valid_window(state.input_window)
    local root = panel_open and state.root
        or select(1, find_root(api.nvim_get_current_buf()))
    if not root then
        vim.notify(
            "Codex: no project root found for the chat history.",
            vim.log.levels.ERROR
        )
        return
    end

    local chats = chats_for_root(root)
    if #chats == 0 then
        vim.notify("Codex: no cached chats for this project yet.", vim.log.levels.INFO)
        return
    end
    vim.ui.select(chats, {
        prompt = "Codex chats · " .. vim.fn.fnamemodify(root, ":t"),
        kind = "flecha_codex_chat",
        format_item = function(chat)
            local updated = os.date("%Y-%m-%d %H:%M", chat.updated_at or 0)
            return updated .. "  " .. (chat.title or "Untitled chat")
        end,
    }, function(chat)
        if not chat then
            return
        end
        if not valid_window(state.transcript_window)
            and not valid_window(state.input_window)
        then
            open_panel()
        end
        if canonical(state.root) ~= canonical(root) then
            vim.notify("Codex: the active project changed before the chat opened.", vim.log.levels.ERROR)
            return
        end

        state.thread_id = chat.id
        state.messages = {}
        for _, message in ipairs(type(chat.messages) == "table" and chat.messages or {}) do
            if (message.role == "user" or message.role == "assistant")
                and type(message.text) == "string"
            then
                table.insert(state.messages, {
                    role = message.role,
                    text = message.text,
                })
            end
        end
        state.chat_title = chat.title
        state.chat_created_at = chat.created_at
        render_conversation()
        render_status("↺ Resumed chat")
        focus_input()
    end)
end

function M.new_chat()
    if state.job then
        M.stop()
    end
    state.thread_id = nil
    state.messages = {}
    state.chat_title = nil
    state.chat_created_at = nil
    if not valid_window(state.transcript_window)
        and not valid_window(state.input_window)
    then
        open_panel()
    end
    render_welcome()
    render_status("New chat")
    focus_input()
end

function M.setup(configuration)
    configuration = configuration or {}
    if configuration.command ~= nil
        and (type(configuration.command) ~= "table"
            or type(configuration.command[1]) ~= "string")
    then
        error("flecha.codex: command must be a non-empty list of strings")
    end
    options = vim.tbl_extend("force", {}, defaults, configuration)
    configured = true

    api.nvim_set_hl(0, "FlechaCodexNormal", { default = true, link = "NormalFloat" })
    api.nvim_set_hl(0, "FlechaCodexTitle", { default = true, link = "Title" })
    api.nvim_set_hl(0, "FlechaCodexPrompt", { default = true, link = "Function" })
    api.nvim_set_hl(0, "FlechaCodexMuted", { default = true, link = "Comment" })
    api.nvim_set_hl(0, "FlechaCodexStatus", { default = true, link = "DiagnosticInfo" })
end

function M.is_configured()
    return configured
end

function M.info()
    local resolved = resolve_command()
    return {
        root = state.root,
        root_kind = state.root_kind,
        skip_git_repo_check = state.skip_git_repo_check,
        thread_id = state.thread_id,
        cache_path = normalize(options.cache_path),
        running = state.job ~= nil,
        status = state.status,
        command = vim.deepcopy(options.command),
        executable = resolved and resolved[1] or nil,
    }
end

-- Test seams kept private from the documented plugin API.
M._state = state
M._find_root = find_root
M._handle_event = handle_event
M._command_for_turn = command_for_turn
M._resolve_command = resolve_command
M._set_transport = function(value)
    transport = value
end

return M
