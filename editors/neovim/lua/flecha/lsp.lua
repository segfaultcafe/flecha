local M = {}

local uv = vim.uv or vim.loop
local configured = false
local options = {}
local warned = false

local defaults = {
    enabled = true,
    root_markers = { "longbow.toml", ".git" },
}

local function dirname(path)
    return vim.fn.fnamemodify(path, ":h")
end

local function basename(path)
    return vim.fn.fnamemodify(path, ":t")
end

local function join(...)
    local parts = { ... }
    local path = table.remove(parts, 1) or ""
    for _, part in ipairs(parts) do
        path = path:gsub("[/\\]+$", "") .. "/" .. part:gsub("^[/\\]+", "")
    end
    return path
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

local function executable_name(name)
    if vim.fn.has("win32") == 1 then
        return name .. ".exe"
    end
    return name
end

local function is_executable(path)
    return path ~= nil and vim.fn.executable(path) == 1
end

local function executable_override(value)
    if not value or value == "" then
        return nil
    end

    if not value:find("[/\\]") then
        local from_path = vim.fn.exepath(value)
        if from_path ~= "" then
            return normalize(from_path)
        end
    end

    local path = normalize(value)
    return is_executable(path) and path or nil
end

local function runtime_root()
    local files = vim.api.nvim_get_runtime_file("lua/flecha/lsp.lua", false)
    if #files == 0 then
        return nil
    end
    local normalized = normalize(files[1])
    local module_path = uv.fs_realpath(normalized) or normalized
    return dirname(dirname(dirname(module_path)))
end

local function bundled_tools(plugin_root)
    if not plugin_root or basename(plugin_root) ~= "neovim" then
        return nil, nil
    end

    local editors = dirname(plugin_root)
    local flecha_share = dirname(editors)
    local share = dirname(flecha_share)
    if basename(editors) ~= "editors"
        or basename(flecha_share) ~= "flecha"
        or basename(share) ~= "share"
    then
        return nil, nil
    end

    local prefix = dirname(share)
    local server = join(prefix, "bin", executable_name("flecha-lsp"))
    local compiler = join(prefix, "bin", executable_name("flecha"))
    if is_executable(server) and is_executable(compiler) then
        return normalize(server), normalize(compiler)
    end
    return nil, nil
end

local function sibling_tool(path, name)
    if not path then
        return nil
    end
    local candidate = join(dirname(path), executable_name(name))
    return is_executable(candidate) and normalize(candidate) or nil
end

local function path_tool(name)
    local from_path = vim.fn.exepath(executable_name(name))
    return from_path ~= "" and normalize(from_path) or nil
end

local function resolve_tools(opts, plugin_root)
    local configured_server = opts.server_path or (opts.cmd and opts.cmd[1])
    local server = executable_override(configured_server)
    local compiler = executable_override(opts.compiler_path)

    -- A release bundle is selected only when its server and compiler are both
    -- present. This keeps independently installed Flecha versions from being
    -- mixed accidentally.
    if not opts.server_path and not opts.compiler_path and not opts.cmd then
        server, compiler = bundled_tools(plugin_root or runtime_root())
    end

    if not server and compiler then
        server = sibling_tool(compiler, "flecha-lsp")
    elseif not compiler and server then
        compiler = sibling_tool(server, "flecha")
    end

    if not server and not compiler then
        -- Prefer the language server shipped beside the selected compiler.
        -- This prevents an obsolete standalone flecha-lsp earlier on PATH
        -- from being paired with a newer Flecha installation.
        local path_compiler = path_tool("flecha")
        local path_server = sibling_tool(path_compiler, "flecha-lsp")
        if path_server then
            return path_server, path_compiler
        end

        path_server = path_tool("flecha-lsp")
        path_compiler = sibling_tool(path_server, "flecha")
        if path_compiler then
            return path_server, path_compiler
        end
    end

    if not server or not compiler then
        return nil, nil
    end
    return server, compiler
end

local function canonical(path)
    path = normalize(path)
    return path and (uv.fs_realpath(path) or path) or nil
end

local function find_root(bufnr, markers)
    local name = vim.api.nvim_buf_get_name(bufnr)
    local start = name ~= "" and dirname(normalize(name))
        or canonical(vim.fn.getcwd())
    local directory = start

    while directory do
        for _, marker in ipairs(markers) do
            if uv.fs_stat(join(directory, marker)) then
                return canonical(directory)
            end
        end
        local parent = dirname(directory)
        if parent == directory then
            break
        end
        directory = parent
    end

    return canonical(start)
end

local function notify_missing()
    if warned then
        return
    end
    warned = true
    vim.schedule(function()
        vim.notify(
            "Flecha LSP: flecha-lsp and flecha were not found; install them together or configure vim.g.flecha_lsp",
            vim.log.levels.WARN
        )
    end)
end

local function start(bufnr)
    if not options.enabled
        or type(vim.lsp.start) ~= "function"
        or vim.bo[bufnr].filetype ~= "flecha"
    then
        return
    end

    local root = find_root(bufnr, options.root_markers)
    local get_clients = vim.lsp.get_clients or vim.lsp.get_active_clients
    local active_clients = get_clients()
    for _, client in ipairs(active_clients) do
        local client_command = client.config.cmd
        local client_executable = type(client_command) == "table"
            and client_command[1]
            or nil
        local executable = client_executable
            and basename(client_executable):lower()
            or ""
        local is_flecha = client.name == "flecha"
            or executable == "flecha-lsp"
            or executable == "flecha-lsp.exe"
        if is_flecha and (vim.lsp.buf_is_attached(bufnr, client.id)
            or canonical(client.config.root_dir) == canonical(root))
        then
            if not vim.lsp.buf_is_attached(bufnr, client.id) then
                vim.lsp.buf_attach_client(bufnr, client.id)
            end
            return client.id
        end
    end

    local server, compiler = resolve_tools(options)
    if not server or not compiler then
        notify_missing()
        return
    end

    local command = vim.deepcopy(options.cmd or { server })
    command[1] = executable_override(command[1]) or command[1]

    local config = {
        name = "flecha",
        cmd = command,
        cmd_env = { FLECHA_COMPILER = compiler },
        root_dir = root,
        capabilities = options.capabilities,
        init_options = options.init_options,
        settings = options.settings,
        on_attach = options.on_attach,
    }

    vim.lsp.start(config, {
        bufnr = bufnr,
        silent = true,
        reuse_client = function(client, candidate)
            return client.name == candidate.name
                and canonical(client.config.root_dir)
                    == canonical(candidate.root_dir)
        end,
    })
end

function M.setup(opts)
    opts = opts or {}
    if opts.cmd ~= nil
        and (type(opts.cmd) ~= "table" or type(opts.cmd[1]) ~= "string")
    then
        error("flecha.lsp: cmd must be a non-empty list of strings")
    end
    options = vim.tbl_extend("force", {}, defaults, opts)
    configured = true
    warned = false

    local group = vim.api.nvim_create_augroup("FlechaLsp", { clear = true })
    if options.enabled then
        vim.api.nvim_create_autocmd("FileType", {
            group = group,
            pattern = "flecha",
            callback = function(event)
                start(event.buf)
            end,
            desc = "Start the Flecha language server",
        })

        local current = vim.api.nvim_get_current_buf()
        if vim.bo[current].filetype == "flecha" then
            start(current)
        end
    end
end

function M.is_configured()
    return configured
end

function M.info(bufnr)
    bufnr = bufnr or vim.api.nvim_get_current_buf()
    local server, compiler = resolve_tools(options)
    return {
        enabled = options.enabled,
        server = server,
        compiler = compiler,
        root = find_root(bufnr, options.root_markers),
    }
end

-- Exposed for deterministic headless tests; not part of the public API.
M._find_root = find_root
M._resolve_tools = resolve_tools

return M
