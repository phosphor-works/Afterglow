-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local providers = require("providers")
local history = require("history")
local script = {}
script.metadata = {name="Project navigator"}
local config, home, saved, revision
local busy = false
-- The arguments of each command, for its usage line. The handler cannot read
-- the manifest, so these repeat its usage fields.
local Usage = {open="open DIR [LAYOUT]", list="list", pick="pick", favorite="favorite", previous="previous",
    import="import < DIRECTORIES"}
local MaxFavorites = 100
-- realpath takes the paths after its three own arguments, and a job has at most 64.
local MaxImport = 61
script.migrate = history.migrate
function script.on_load(ev)
    config, home = ev.package.config, ev.package.home
    saved, revision = history.normalize(ev.storage.value), ev.storage.revision
end
-- A failure goes to stderr and ends the run with status 1. A run with no
-- client (the palette, a key) shows it as a notification.
local function fail(ev, message)
    return {afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1)}
end
local function usage(ev, command)
    return {afterglow.output_error(ev, "usage: aglow run project-navigator " .. Usage[command] .. "\n"), afterglow.exit(ev, 2)}
end
-- A message for the user: on stdout for a run from the shell, else a
-- notification on the pane.
local function notice(ev, message)
    if ev.source == "shell" then return afterglow.output(ev, message .. "\n") end
    if ev.pane then return afterglow.notify("Project navigator", message, ev.pane.id) end
    return nil
end
-- A wait that ended because the run ended: a cancel, or a client that left.
-- Nothing may open after it.
local function cancelled(err)
    return err ~= nil and err.code == "cancelled"
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
-- The directories with their symbolic links resolved, as Afterglow names a
-- project directory: a shell reports its directory through the links that it
-- followed, so one directory could be saved under two names. realpath -m
-- also takes a path that does not exist. Without realpath, a path stays as it
-- is. Returns nil when the run ended during the wait.
local function canonical(paths)
    if #paths == 0 then return {} end
    -- -z ends each path with a NUL, which no path holds: a line break in a
    -- link target cannot shift the paths after it.
    local argv = {"realpath", "-mz", "--"}
    for _, path in ipairs(paths) do table.insert(argv, path) end
    -- Room for every path at the 4,096-byte limit, so a long batch is not cut.
    local resolved, err = afterglow.await(afterglow.exec("realpath", argv,
        {timeout_ms=2000, max_bytes=MaxImport * 4097}))
    if cancelled(err) then return nil end
    local lines = {}
    for line in string.gmatch(resolved and resolved.stdout or "", "(%Z*)%z") do table.insert(lines, line) end
    local out = {}
    for i, path in ipairs(paths) do
        out[i] = #lines == #paths and providers.path(lines[i], home) or path
    end
    return out
end
-- launch opens the item with layout, and stores template as the directory's
-- layout for the next time. A nil template keeps the stored one.
local function launch(ev, item, layout, template)
    local opened, err
    if item.kind == "project" then
        opened, err = afterglow.await(afterglow.open_project(item.value))
    else
        opened, err = afterglow.await(afterglow.open_directory(item.value, layout))
    end
    if not opened then return fail(ev, err.message) end
    -- A directory that is open already keeps its window, so the layout did not apply.
    local unused = opened.reused and template ~= nil and template ~= ""
    if opened.reused then template = nil end
    if opened.cwd then item.value = opened.cwd end
    saved = history.remember(saved, item, template)
    local failed = save()
    if failed then return fail(ev, "History was not saved: " .. failed) end
    if unused then return notice(ev, "The directory is open already. Its window keeps its layout.") end
    return nil
end
-- Everything to offer: saved and running projects, configured roots, and the
-- results of the provider commands, which run together. Nil when the run ended
-- during the wait.
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
    for _, result in ipairs(results) do
        if cancelled(result.error) then return nil end
    end
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
local function favoriteCount()
    local count = 0
    for _ in pairs(saved.favorites) do count += 1 end
    return count
end
local function favorite(ev, current)
    local path = providers.path(current, home)
    if not path then return fail(ev, "The current pane has no local directory.") end
    local resolved = canonical({path})
    if not resolved then return nil end
    path = resolved[1]
    if #path > history.MaxKey then
        return fail(ev, "This directory path is too long to save as a favorite (over 256 bytes).")
    end
    if not saved.favorites[path] and favoriteCount() >= MaxFavorites then
        return fail(ev, "Favorite limit reached (100). Unpin another directory first.")
    end
    saved.favorites[path] = not saved.favorites[path] or nil
    saved = history.normalize(saved)
    local message = saved.favorites[path] and "Favorite saved" or "Favorite removed"
    local failed = save()
    if failed then return fail(ev, "History was not saved: " .. failed) end
    return notice(ev, message)
end
-- A directory from the command line: relative to where aglow run ran.
-- Returns nil and a message for a path that cannot be a directory.
local function resolve(path, cwd)
    if path == "~" or string.sub(path, 1, 2) == "~/" then path = home .. string.sub(path, 2)
    elseif string.sub(path, 1, 1) == "~" then
        -- The shell expands ~user. A script cannot look up the home of another user.
        return nil, "~user is not expanded here. Give the full path."
    elseif string.sub(path, 1, 1) ~= "/" and cwd ~= "" then path = cwd .. "/" .. path end
    -- Without trailing slashes, as stored: "DIR/" and "DIR" are one directory.
    local normal = providers.path(path, home)
    if not normal then return nil, "The directory must be an absolute path of at most 4,096 bytes." end
    return normal
end
-- import: a directory on each line of stdin becomes a favorite.
local function import(ev)
    if ev.stdin == nil then return fail(ev, "Give the directories on stdin, one on each line.") end
    local paths, skipped, lines = {}, 0, 0
    local more = false
    for line, after in string.gmatch(ev.stdin, "([^\r\n]+)()") do
        lines += 1
        local path = resolve(line, ev.cwd)
        if path then table.insert(paths, path) else skipped += 1 end
        if lines == MaxImport then
            -- Only the first lines are read, so a large stdin stays within the
            -- time limit of the handler: the rest is not even counted.
            more = string.find(ev.stdin, "[^\r\n]", after) ~= nil
            break
        end
    end
    local resolved = canonical(paths)
    if not resolved then return nil end
    local added = 0
    for _, path in ipairs(resolved) do
        if saved.favorites[path] then continue end
        if #path > history.MaxKey or favoriteCount() >= MaxFavorites then
            skipped += 1
        else
            saved.favorites[path] = true
            added += 1
        end
    end
    saved = history.normalize(saved)
    local failed = save()
    if failed then return fail(ev, "History was not saved: " .. failed) end
    local effects = {notice(ev, "Added " .. added .. (added == 1 and " favorite" or " favorites"))}
    if skipped > 0 then
        table.insert(effects, afterglow.output_error(ev, "Skipped " .. skipped .. (skipped == 1 and " line" or " lines")
            .. ": not an absolute path, a path over 256 bytes, or over the limit of 100 favorites.\n"))
    end
    if more then
        table.insert(effects, afterglow.output_error(ev, "Read only the first " .. MaxImport .. " lines.\n"))
    end
    if skipped > 0 or more then table.insert(effects, afterglow.exit(ev, 1)) end
    return effects
end
-- Favorites, then the recent history, then the saved layouts: KIND<TAB>VALUE
-- lines. It reads the history only, so it runs beside another operation.
local function list(ev, value)
    local lines, skipped = {}, 0
    local function add(kind, text)
        -- A tab or a line break in a value would break the line format.
        if string.find(text, "[\t\r\n]") then skipped += 1 else table.insert(lines, kind .. "\t" .. text) end
    end
    local favorites = {}
    for path in pairs(value.favorites) do table.insert(favorites, path) end
    table.sort(favorites)
    for _, path in ipairs(favorites) do add("favorite", path) end
    for _, item in ipairs(value.recent) do add(item.kind == "project" and "recent-layout" or "recent", item.value) end
    local projects, err = afterglow.await(afterglow.list_projects())
    for _, name in ipairs(projects and projects.names or {}) do add("layout", name) end
    local effects = {}
    if #lines > 0 then table.insert(effects, afterglow.output(ev, table.concat(lines, "\n") .. "\n")) end
    if skipped > 0 then
        table.insert(effects, afterglow.output_error(ev, "Not listed: " .. skipped .. " entries with a tab or a line break.\n"))
    end
    if not projects then
        table.insert(effects, afterglow.output_error(ev, "The saved layouts are not listed: " .. err.message .. "\n"))
        table.insert(effects, afterglow.exit(ev, 1))
    end
    return effects
end
local function navigate(command, ev)
    local stored, err, result = afterglow.await(afterglow.state_read())
    if not stored then return fail(ev, err.message) end
    if command == "list" then return list(ev, history.normalize(stored.value)) end
    saved, revision = history.normalize(stored.value), stored.revision
    if command == "import" then return import(ev) end
    if command == "open" then
        local directory, problem = resolve(ev.args[1], ev.cwd)
        if not directory then
            return {afterglow.output_error(ev, problem .. "\n"), afterglow.exit(ev, 2)}
        end
        local resolved = canonical({directory})
        if not resolved then return nil end
        directory = resolved[1]
        -- Only a layout that the command names becomes the directory's template.
        return launch(ev, {kind="directory", value=directory}, ev.args[2] or saved.templates[directory], ev.args[2])
    end
    if not ev.pane then return fail(ev, "There is no pane to act on.") end
    -- The pane as it is now, after the wait.
    local pane
    for _, candidate in ipairs(result.terminal.panes) do
        if candidate.id == ev.pane.id then pane = candidate end
    end
    if not pane then return fail(ev, "The pane has closed.") end
    local current, currentProject = pane.cwd, nil
    for _, window in ipairs(result.terminal.windows) do
        if window.id == pane.window then
            current = window.project_directory ~= "" and window.project_directory or pane.cwd
            currentProject = window.project
        end
    end
    if command == "favorite" then return favorite(ev, current) end
    if command == "previous" then
        for _, item in ipairs(saved.recent) do
            if (item.kind == "directory" and item.value ~= current)
                or (item.kind == "project" and item.value ~= currentProject) then
                return launch(ev, item, saved.templates[item.value], nil)
            end
        end
        return fail(ev, "No previous project yet.")
    end
    local entries, projects, failed = candidates(result.terminal)
    if not entries then return nil end
    local item, problem = pick(pane.id, entries, failed)
    if not item then
        if problem then return fail(ev, problem) end
        return nil
    end
    local layout, failure = layoutFor(pane.id, item, projects)
    if layout == nil then
        if failure then return fail(ev, failure) end
        return nil
    end
    -- A layout chosen in the picker becomes the template. A running directory asks for none.
    return launch(ev, item, layout, (item.kind == "directory" and not item.running) and layout or nil)
end
function script.run_command(command, ev)
    if not Usage[command] then return nil end
    local arguments = #ev.args
    if (command == "open" and (arguments < 1 or arguments > 2)) or (command ~= "open" and arguments > 0) then
        return usage(ev, command)
    end
    if command == "list" then return navigate(command, ev) end
    -- One operation at a time: each one reads and writes the saved history.
    if busy then return fail(ev, "A project operation is already open.") end
    busy = true
    local ok, effects = pcall(navigate, command, ev)
    busy = false
    if not ok then error(effects, 0) end
    return effects
end
return script
