-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local script = {}
script.metadata = {name = "fzf picker"}
local config
-- The select dialog takes at most this many entries.
local MaxDialogItems = 500
local Title = "Pick a project or file"

function script.on_load(ev)
    config = ev.package.config
end

local function lines(text)
    local out = {}
    for line in string.gmatch(text or "", "[^\n]+") do
        table.insert(out, line)
    end
    return out
end

-- A path the shell reads as one word.
local function quote(path)
    return "'" .. string.gsub(path, "'", "'\\''") .. "'"
end

-- The saved projects, then the files under dir: one entry each.
local function entries(dir)
    local items = {}
    local projects = afterglow.await(afterglow.list_projects())
    for _, name in ipairs(projects and projects.names or {}) do
        table.insert(items, "project: " .. name)
    end
    if type(dir) == "string" and string.sub(dir, 1, 1) == "/" then
        -- find exits with 1 when it cannot read a directory: what it found still counts.
        local found, _, result = afterglow.await(afterglow.exec("files",
            {"find", ".", "-maxdepth", tostring(config.max_depth), "-type", "f", "-not", "-path", "*/.git/*"},
            {cwd = dir, max_bytes = 1048576}))
        local data = found or result.data or {}
        for _, line in ipairs(lines(data.stdout)) do
            if #items >= config.max_files then break end
            table.insert(items, "file: " .. (string.gsub(line, "^%./", "")))
        end
    end
    return items
end

-- What a choice does: a project opens, a file's path goes to the pane's prompt.
local function act(ev, choice)
    if type(choice) ~= "string" then return nil end
    local project = string.match(choice, "^project: (.+)$")
    if project then return afterglow.open_project(project) end
    local file = string.match(choice, "^file: (.+)$")
    if file and ev.pane then return afterglow.send_text(ev.pane, quote(file) .. " ") end
    return nil
end

function script.run_command(_, ev)
    if not ev.pane then
        return {afterglow.output_error(ev, "fzf-picker needs a pane\n"), afterglow.exit(ev, 1)}
    end
    local dir = ev.cwd ~= "" and ev.cwd or ev.pane.cwd
    local items = entries(dir)
    if #items == 0 then
        return afterglow.notify("fzf picker", "No projects or files to pick from.", ev.pane.id)
    end
    -- fzf when it runs: its list on stdin, its screen on the pane, its choice on stdout.
    if afterglow.await(afterglow.exec("fzf", {config.fzf, "--version"})) then
        local picked = afterglow.await(afterglow.overlay(ev.pane, {config.fzf, "--layout=reverse", "--prompt=pick> "},
            {stdin = table.concat(items, "\n"), cwd = dir, title = Title, width = "80%", height = "60%"}))
        -- Esc (130), no match (1) and a cancelled run choose nothing.
        if not picked or picked.exit_code ~= 0 then return nil end
        return act(ev, (string.gsub(picked.stdout, "\n$", "")))
    end
    -- Without fzf: the select dialog, with the first entries.
    local list = {}
    for i, item in ipairs(items) do
        if i > MaxDialogItems then break end
        table.insert(list, {id = tostring(i), title = string.sub(item, 1, 512)})
    end
    local choice = afterglow.await(afterglow.select(ev.pane, Title, list))
    return act(ev, choice and items[tonumber(choice.id)])
end

return script
