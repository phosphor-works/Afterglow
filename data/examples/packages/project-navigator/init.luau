-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local providers = require("providers")
local history = require("history")
local script = {}
script.metadata = {name="Project navigator"}
local config, home, saved, revision
local busy = false
script.migrate = history.migrate
function script.on_load(ev)
    config, home = ev.package.config, ev.package.home
    saved, revision = history.normalize(ev.storage.value), ev.storage.revision
end
-- A message for the user: on stdout or stderr for a run from the shell, else
-- a notification on the pane.
local function notice(ev, message, failed)
    if ev.source == "shell" then
        if failed then return {afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1)} end
        return afterglow.output(ev, message .. "\n")
    end
    return afterglow.notify("Project navigator", message, ev.pane and ev.pane.id)
end
-- Cuts text to at most max bytes without splitting a UTF-8 sequence.
local function trim(text, max)
    if #text <= max then return text end
    return string.sub(text, 1, utf8.offset(text, 0, max + 1) - 1)
end
-- The data of an awaited selector, nil when it is cancelled, or nil and a message
-- when it could not open (another dialog in the window is "busy").
local function choose(pane, title, items)
    local choice, err = afterglow.await(afterglow.select(pane, title, items))
    if choice then return choice end
    if not err or err.code == "cancelled" then return nil end
    return nil, err.message
end
-- Writes the history at the revision it was read. Returns an error message on failure.
local function save()
    local written, err = afterglow.await(afterglow.state_write(saved, revision))
    if not written then return err.message end
    revision = written.revision
    return nil
end
local function launch(ev, item, layout)
    local opened, err
    if item.kind == "project" then
        opened, err = afterglow.await(afterglow.open_project(item.value))
    else
        opened, err = afterglow.await(afterglow.open_directory(item.value, layout))
    end
    if not opened then return notice(ev, err.message, true) end
    if opened.cwd then item.value = opened.cwd end
    saved = history.remember(saved, item, layout)
    local failed = save()
    if failed then return notice(ev, "History was not saved: " .. failed, true) end
    return nil
end
-- Everything to offer: saved and running projects, configured roots, and the
-- results of the provider commands, which run together.
local function candidates(terminal)
    local entries, count = {}, 0
    local function add(kind, value, running)
        if type(value) ~= "string" or value == "" then return end
        local key = kind .. ":" .. value
        if entries[key] then
            if running then entries[key].running = running end
        elseif count < 400 then
            count += 1
            entries[key] = {kind=kind, value=value, running=running}
        end
    end
    for path in pairs(saved.favorites) do add("directory", path) end
    for _, item in ipairs(saved.recent) do add(item.kind, item.value) end
    for _, window in ipairs(terminal.windows) do
        if window.project_directory and window.project_directory ~= "" then add("directory", window.project_directory, true) end
        if window.project and window.project ~= "" then add("project", window.project, true) end
    end
    local jobs, roots = providers.jobs(config, home)
    for _, root in ipairs(roots) do add("directory", root) end
    local effects = {afterglow.list_projects()}
    for _, job in ipairs(jobs) do
        table.insert(effects, afterglow.exec(job.id, job.command, {timeout_ms=5000, max_bytes=131072}))
    end
    local results = afterglow.await_all(effects)
    local projects = results[1].ok and results[1].data.names or {}
    for _, name in ipairs(projects) do add("project", name) end
    local failed = {}
    for i, job in ipairs(jobs) do
        local result = results[i + 1]
        -- A provider that stopped at the output limit, or exited non-zero for a
        -- missing root or an unreadable directory, still returns the complete
        -- records it printed. Any other failure (no start, a timeout, a refused
        -- job) returns no output, so the title names it.
        local stdout = result.data and result.data.stdout
        if result.ok or stdout then
            for _, path in ipairs(providers.parse(job.kind, stdout or "", home)) do add("directory", path) end
        else
            local code = result.error and result.error.code or ""
            local reason = code == "spawn_failed" and "not started" or code == "timeout" and "timed out" or code
            table.insert(failed, job.id .. " " .. reason)
        end
    end
    return entries, projects, failed
end
-- The chosen entry, nil when the picker is cancelled, or nil and a message.
local function pick(pane, entries, failed)
    local sorted = {}
    for _, item in pairs(entries) do
        local score = item.running and 100 or 0
        if item.kind == "directory" and saved.favorites[item.value] then score += 1000 end
        for i, recent in ipairs(saved.recent) do
            if recent.kind == item.kind and recent.value == item.value then score += 100 - i; break end
        end
        item.score = score
        table.insert(sorted, item)
    end
    table.sort(sorted, function(a, b)
        if a.score ~= b.score then return a.score > b.score end
        return a.value < b.value
    end)
    local items, choices = {}, {}
    for i, item in ipairs(sorted) do
        local id = tostring(i)
        choices[id] = item
        local label = item.kind == "project" and ("Layout: " .. item.value) or item.value
        if item.running then label = "[running] " .. label end
        if item.kind == "directory" and saved.favorites[item.value] then label = "★ " .. label end
        table.insert(items, {id=id, title=trim(label, 500)})
    end
    if #items == 0 then return nil, "No projects found. Configure roots or save a project layout first." end
    local title = "Navigate projects"
    if #failed > 0 then title ..= " (unavailable: " .. table.concat(failed, ", ") .. ")" end
    local choice, problem = choose(pane, title, items)
    if not choice then return nil, problem end
    if not choices[choice.id] then return nil, "Selection expired" end
    return choices[choice.id]
end
-- The layout to open the entry with, nil when the picker is cancelled, or nil and a message.
local function layoutFor(pane, item, projects)
    -- A project opens its own layout, and a running directory is reused, so
    -- neither asks. Without a stored template this is "", because nil means
    -- that the picker was cancelled.
    if item.kind == "project" or item.running then return saved.templates[item.value] or "" end
    local layouts, names = {{id="shell", title="Start a shell"}}, {shell=""}
    local previous = saved.templates[item.value]
    for i, name in ipairs(projects) do
        local id = "layout-" .. i
        names[id] = name
        local choice = {id=id, title=name .. (name == previous and " (last used)" or "")}
        if name == previous then table.insert(layouts, 1, choice) else table.insert(layouts, choice) end
        if #layouts >= 100 then break end
    end
    if #layouts == 1 then return "" end
    local choice, problem = choose(pane, "Layout for " .. providers.label(item.value), layouts)
    if not choice then return nil, problem end
    if names[choice.id] == nil then return nil, "Selection expired" end
    return names[choice.id]
end
local function favorite(ev, current)
    local path = providers.path(current, home)
    if not path then return notice(ev, "The current pane has no local directory.", true) end
    if #path > history.MaxKey then
        return notice(ev, "This directory path is too long to save as a favorite (over 256 bytes).", true)
    end
    if not saved.favorites[path] then
        local count = 0
        for _ in pairs(saved.favorites) do count += 1 end
        if count >= 100 then return notice(ev, "Favorite limit reached (100). Unpin another directory first.", true) end
    end
    saved.favorites[path] = not saved.favorites[path] or nil
    saved = history.normalize(saved)
    local message = saved.favorites[path] and "Favorite saved" or "Favorite removed"
    local failed = save()
    if failed then return notice(ev, "History was not saved: " .. failed, true) end
    return notice(ev, message)
end
-- A directory from the command line: relative to where afterglow run ran.
local function resolve(path, cwd)
    if path == "~" or string.sub(path, 1, 2) == "~/" then return home .. string.sub(path, 2) end
    if string.sub(path, 1, 1) == "/" or cwd == "" then return path end
    return cwd .. "/" .. path
end
-- Favorites, then the recent history, then the saved layouts: KIND<TAB>VALUE lines.
local function list(ev)
    local lines, favorites = {}, {}
    for path in pairs(saved.favorites) do table.insert(favorites, path) end
    table.sort(favorites)
    for _, path in ipairs(favorites) do table.insert(lines, "favorite\t" .. path) end
    for _, item in ipairs(saved.recent) do
        table.insert(lines, (item.kind == "project" and "recent-layout\t" or "recent\t") .. item.value)
    end
    local projects = afterglow.await(afterglow.list_projects())
    for _, name in ipairs(projects and projects.names or {}) do table.insert(lines, "layout\t" .. name) end
    if #lines == 0 then return nil end
    return afterglow.output(ev, table.concat(lines, "\n") .. "\n")
end
local function navigate(command, ev)
    local pane = ev.pane and ev.pane.id
    local stored, err, result = afterglow.await(afterglow.state_read())
    if not stored then return notice(ev, err.message, true) end
    saved, revision = history.normalize(stored.value), stored.revision
    if command == "list" then return list(ev) end
    if command == "open" then
        local directory = ev.args[1]
        if not directory or #ev.args > 2 then
            return {afterglow.output_error(ev, "usage: afterglow run project-navigator open DIR [LAYOUT]\n"), afterglow.exit(ev, 2)}
        end
        directory = resolve(directory, ev.cwd)
        return launch(ev, {kind="directory", value=directory}, ev.args[2] or saved.templates[directory] or "")
    end
    if not pane then return notice(ev, "There is no pane to act on.", true) end
    local current, currentProject = ev.pane.cwd, nil
    for _, window in ipairs(result.terminal.windows) do
        if window.id == ev.pane.window then
            current = window.project_directory ~= "" and window.project_directory or ev.pane.cwd
            currentProject = window.project
        end
    end
    if command == "favorite" then return favorite(ev, current) end
    if command == "previous" then
        for _, item in ipairs(saved.recent) do
            if (item.kind == "directory" and item.value ~= current)
                or (item.kind == "project" and item.value ~= currentProject) then
                return launch(ev, item, saved.templates[item.value])
            end
        end
        return notice(ev, "No previous project yet.", true)
    end
    local entries, projects, failed = candidates(result.terminal)
    local item, problem = pick(pane, entries, failed)
    if not item then
        if problem then return notice(ev, problem, true) end
        return nil
    end
    local layout, failure = layoutFor(pane, item, projects)
    if layout == nil then
        if failure then return notice(ev, failure, true) end
        return nil
    end
    return launch(ev, item, layout)
end
function script.run_command(command, ev)
    if command == "list" then return navigate(command, ev) end
    -- One operation at a time: each one reads and writes the saved history.
    if busy then return notice(ev, "A project operation is already open.", true) end
    busy = true
    local ok, effects = pcall(navigate, command, ev)
    busy = false
    if not ok then error(effects, 0) end
    return effects
end
return script
