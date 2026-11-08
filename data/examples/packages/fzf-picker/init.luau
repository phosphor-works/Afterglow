-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local script = {}
script.metadata = {name = "fzf picker"}
local config
-- The select dialog takes at most this many entries, with titles of at most
-- this many UTF-16 units.
local MaxDialogItems = 500
local MaxDialogTitle = 512
local Title = "Pick a project or file"
local Replacement = "\u{FFFD}"

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

-- A name that is safe to list and to type: no control characters, which the
-- shell would read as keys (Ctrl+D, Enter), and no U+FFFD, which stands for
-- bytes that were not UTF-8, so the name is not the file's.
local function usable(name)
    return not string.find(name, "%c") and not string.find(name, Replacement, 1, true)
end

-- A path that a POSIX shell reads as one word.
local function quote(path)
    return "'" .. string.gsub(path, "'", "'\\''") .. "'"
end

-- At most max UTF-16 units, cut between characters.
local function clip(text, max)
    local units = 0
    for position, code in utf8.codes(text) do
        units += if code > 0xFFFF then 2 else 1
        if units > max then
            return string.sub(text, 1, position - 1)
        end
    end
    return text
end

-- The pane in a snapshot, nil when it has closed.
local function paneIn(terminal, id)
    for _, pane in ipairs(terminal and terminal.panes or {}) do
        if pane.id == id then
            return pane
        end
    end
    return nil
end

-- The saved projects, then at most max_files files under dir: one entry each.
local function entries(ev, dir)
    local items = {}
    local projects = afterglow.await(afterglow.list_projects())
    for _, name in ipairs(projects and projects.names or {}) do
        if usable(name) then
            table.insert(items, "project: " .. name)
        end
    end
    if type(dir) == "string" and string.sub(dir, 1, 1) == "/" then
        -- .git and names with control characters are pruned: find does not go into them.
        -- find exits with 1 when it cannot read a directory: what it found still counts.
        local found, err, result = afterglow.await(afterglow.exec("files:" .. ev.run,
            {"find", ".", "-maxdepth", tostring(config.max_depth), "(", "-name", ".git", "-o", "-name", "*[[:cntrl:]]*", ")",
             "-prune", "-o", "-type", "f", "-print"},
            {cwd = dir, max_bytes = 1048576}))
        local data = found or (result and result.data) or {}
        local names = lines(data.stdout)
        -- At the output limit, the last line can be a part of a name.
        if data.truncated or (err and err.code == "output_limit") then
            table.remove(names)
        end
        local files = 0
        for _, line in ipairs(names) do
            if files >= config.max_files then break end
            local name = string.gsub(line, "^%./", "")
            if usable(name) then
                table.insert(items, "file: " .. name)
                files += 1
            end
        end
    end
    return items
end

-- Ends a pick that has no path. effect, when given, comes first. From a
-- shell, a failure goes to stderr and the run exits with 1, so that
-- `f=$(aglow run fzf-picker pick) && vim "$f"` stops. Otherwise a failure
-- shows as a notification. A cancel has no message.
local function noPath(ev, message, effect)
    local effects = {}
    if effect then table.insert(effects, effect) end
    if ev.source == "shell" then
        if message then table.insert(effects, afterglow.output_error(ev, message .. "\n")) end
        table.insert(effects, afterglow.exit(ev, 1))
    elseif message then
        table.insert(effects, afterglow.notify("fzf picker", message, ev.pane.id))
    end
    return effects
end

-- What a choice does: a project opens. A file's absolute path goes to the
-- client of aglow run, or to the pane's prompt.
local function act(ev, choice, dir, event)
    local project = string.match(choice, "^project: (.+)$")
    if project then return noPath(ev, nil, afterglow.open_project(project)) end
    local file = string.match(choice, "^file: (.+)$")
    if not file or not usable(file) then return noPath(ev) end
    local path = (if string.sub(dir, -1) == "/" then dir else dir .. "/") .. file
    if ev.source == "shell" then
        return afterglow.output(ev, path .. "\n")
    end
    -- The pane as it is now: a program that started meanwhile would get the keys.
    local pane = paneIn(event and event.terminal, ev.pane.id)
    if not pane then return nil end
    if pane.command ~= "" then
        return noPath(ev, "The pane runs " .. pane.command .. ". The path was not typed.")
    end
    return afterglow.send_text(pane, quote(path) .. " ")
end

-- An error tells why, except a cancel: the user chose nothing.
local function failed(ev, err)
    return noPath(ev, if err and err.code ~= "cancelled" then err.message else nil)
end

function script.run_command(_, ev)
    if not ev.pane then
        return {afterglow.output_error(ev, "fzf-picker needs a pane\n"), afterglow.exit(ev, 1)}
    end
    local dir = ev.cwd ~= "" and ev.cwd or ev.pane.cwd
    local items = entries(ev, dir)
    if #items == 0 then
        return noPath(ev, "No projects or files to pick from.")
    end
    -- Only a listed entry acts: the program's output is not typed as it is.
    local listed = {}
    for _, item in ipairs(items) do
        listed[item] = true
    end
    -- fzf when it runs: its list on stdin, its screen on the pane, its choice on stdout.
    if afterglow.await(afterglow.exec("fzf:" .. ev.run, {config.fzf, "--version"})) then
        local picked, err, event = afterglow.await(afterglow.overlay(ev.pane, {config.fzf, "--layout=reverse", "--prompt=pick> "},
            {stdin = table.concat(items, "\n"), cwd = dir, title = Title, width = config.width, height = config.height}))
        if not picked then return failed(ev, err) end
        -- No match (1) and Escape (130) choose nothing. Another status is a
        -- failure, such as 2 for an option that an old fzf does not know.
        if picked.exit_code == 1 or picked.exit_code == 130 then return noPath(ev) end
        -- -1: the status is not known.
        if picked.exit_code == -1 then return noPath(ev, config.fzf .. " ended with a status that is not known.") end
        if picked.exit_code ~= 0 then
            return noPath(ev, config.fzf .. " exited with status " .. tostring(picked.exit_code) .. ".")
        end
        local choice = string.gsub(picked.stdout, "\n$", "")
        if not listed[choice] then return noPath(ev) end
        return act(ev, choice, dir, event)
    end
    -- Without fzf: the select dialog, with the first entries.
    local list = {}
    for i, item in ipairs(items) do
        if i > MaxDialogItems then break end
        table.insert(list, {id = tostring(i), title = clip(item, MaxDialogTitle)})
    end
    local choice, err, event = afterglow.await(afterglow.select(ev.pane, Title, list))
    if not choice then return failed(ev, err) end
    local item = items[tonumber(choice.id)]
    if not item then return noPath(ev) end
    return act(ev, item, dir, event)
end

return script
