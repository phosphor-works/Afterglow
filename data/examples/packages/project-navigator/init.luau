-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local providers = require("providers")
local history = require("history")
local script = {}
script.metadata = {
    name="Project navigator",
    actions={
        {id="open", title="Navigate projects"},
        {id="favorite", title="Toggle favorite project directory"},
        {id="previous", title="Previous project"},
    },
}
local config, home, saved, revision
local busy = false
script.migrate = history.migrate
function script.on_load(ev)
    config, home = ev.package.config, ev.package.home
    saved, revision = history.normalize(ev.storage.value), ev.storage.revision
end
local function notice(message, pane)
    return afterglow.notify("Project navigator", message, pane)
end
-- Writes the history at the revision it was read. Returns an error message on failure.
local function save()
    local written, err = afterglow.await(afterglow.state_write(saved, revision))
    if not written then return err.message end
    revision = written.revision
    return nil
end
local function launch(pane, item, layout)
    local opened, err
    if item.kind == "project" then
        opened, err = afterglow.await(afterglow.open_project(item.value))
    else
        opened, err = afterglow.await(afterglow.open_directory(item.value, layout))
    end
    if not opened then return notice(err.message, pane) end
    if opened.cwd then item.value = opened.cwd end
    saved = history.remember(saved, item, layout)
    local failed = save()
    if failed then return notice("History was not saved: " .. failed, pane) end
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
        if result.ok and result.data.exit_code == 0 then
            for _, path in ipairs(providers.parse(job.kind, result.data.stdout or "", home)) do add("directory", path) end
        else
            table.insert(failed, job.id)
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
        table.insert(items, {id=id, title=string.sub(label, 1, 500)})
    end
    if #items == 0 then return nil, "No projects found. Configure roots or save a project layout first." end
    local title = "Navigate projects"
    if #failed > 0 then title ..= " (unavailable: " .. table.concat(failed, ", ") .. ")" end
    local choice = afterglow.await(afterglow.select(pane, title, items))
    if not choice then return nil end
    if not choices[choice.id] then return nil, "Selection expired" end
    return choices[choice.id]
end
-- The layout to open the entry with, nil when the picker is cancelled, or nil and a message.
local function layoutFor(pane, item, projects)
    if item.kind == "project" or item.running then return saved.templates[item.value] end
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
    local choice = afterglow.await(afterglow.select(pane, "Layout for " .. providers.label(item.value), layouts))
    if not choice then return nil end
    if names[choice.id] == nil then return nil, "Selection expired" end
    return names[choice.id]
end
local function favorite(pane, current)
    local path = providers.path(current, home)
    if not path then return notice("The current pane has no local directory.", pane) end
    if not saved.favorites[path] then
        local count = 0
        for _ in pairs(saved.favorites) do count += 1 end
        if count >= 100 then return notice("Favorite limit reached (100). Unpin another directory first.", pane) end
    end
    saved.favorites[path] = not saved.favorites[path] or nil
    saved = history.normalize(saved)
    local message = saved.favorites[path] and "Favorite saved" or "Favorite removed"
    local failed = save()
    if failed then return notice("History was not saved: " .. failed, pane) end
    return notice(message, pane)
end
local function navigate(action, ev)
    local pane = ev.pane.id
    local stored, err, result = afterglow.await(afterglow.state_read())
    if not stored then return notice(err.message, pane) end
    saved, revision = history.normalize(stored.value), stored.revision
    local current, currentProject = ev.pane.cwd, nil
    for _, window in ipairs(result.terminal.windows) do
        if window.id == ev.pane.window then
            current = window.project_directory ~= "" and window.project_directory or ev.pane.cwd
            currentProject = window.project
        end
    end
    if action == "favorite" then return favorite(pane, current) end
    if action == "previous" then
        for _, item in ipairs(saved.recent) do
            if (item.kind == "directory" and item.value ~= current)
                or (item.kind == "project" and item.value ~= currentProject) then
                return launch(pane, item, saved.templates[item.value])
            end
        end
        return notice("No previous project yet.", pane)
    end
    local entries, projects, failed = candidates(result.terminal)
    local item, problem = pick(pane, entries, failed)
    if not item then
        if problem then return notice(problem, pane) end
        return nil
    end
    local layout, expired = layoutFor(pane, item, projects)
    if layout == nil then
        if expired then return notice(expired, pane) end
        return nil
    end
    return launch(pane, item, layout)
end
function script.run_action(action, ev)
    if not ev.pane then return nil end
    -- One operation at a time: each one reads and writes the saved history.
    if busy then return notice("A project operation is already open.", ev.pane.id) end
    busy = true
    local ok, effects = pcall(navigate, action, ev)
    busy = false
    if not ok then error(effects, 0) end
    return effects
end
return script
