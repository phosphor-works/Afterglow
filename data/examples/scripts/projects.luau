-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copy to the active configuration's scripts/ directory.
-- A minimal project launcher using awaited results and the native selector.
-- It also adds an "open-NAME" command for every saved project, with
-- afterglow.set_commands, so that a key can open one project directly.
-- Commands added at runtime last until the script loads again, so it adds
-- them when it loads and after each choice. A project saved later gets its
-- command at the next choice or load.
local script = {}
script.metadata = {
    id = "projects",
    name = "Projects",
    commands = { open = { summary = "Choose a saved project and open it", palette = "Open a saved project" } },
}
local MaxItems = 500 -- the most items that a selector takes
local MaxCommands = 63 -- a script has at most 64 commands, with "open"
local Prefix = "open-"

-- A failure goes to stderr. A run from the palette shows it as a notification.
local function fail(ev, message)
    return { afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1) }
end

-- The runtime commands: one for each saved project whose command name fits in 64 characters.
local function projectCommands(names)
    local commands, count = {}, 0
    for _, name in names do
        if count == MaxCommands then break end
        if #Prefix + #name <= 64 then
            commands[Prefix .. name] = { summary = "Open the saved project " .. name, palette = "Open project " .. name }
            count += 1
        end
    end
    return afterglow.set_commands(commands)
end

local function open(ev, name)
    local opened, openError = afterglow.await(afterglow.open_project(name))
    if not opened then return fail(ev, openError.message) end
    return { afterglow.output(ev, "Project " .. name .. (opened.reused and " focused" or " opened")
        .. " in window " .. opened.window_id .. "\n") }
end

function script.on_load()
    local projects = afterglow.await(afterglow.list_projects())
    if not projects then return nil end
    return projectCommands(projects.names)
end

function script.run_command(command, ev)
    if #ev.args > 0 then
        return { afterglow.output_error(ev, "usage: aglow run projects " .. command .. "\n"), afterglow.exit(ev, 2) }
    end
    if string.sub(command, 1, #Prefix) == Prefix then
        return open(ev, string.sub(command, #Prefix + 1))
    end
    if command ~= "open" then return nil end
    if not ev.pane then return fail(ev, "There is no pane to show the selector in.") end
    local projects, err = afterglow.await(afterglow.list_projects())
    if not projects then return fail(ev, err.message) end
    local items = {}
    for _, name in projects.names do
        if #items == MaxItems then break end
        table.insert(items, {id=name, title=name})
    end
    if #items == 0 then return fail(ev, "Save a project first using Save Project in the palette.") end
    local title = #projects.names > MaxItems and ("Open project (first " .. MaxItems .. ")") or "Open project"
    local choice, selectError = afterglow.await(afterglow.select(ev.pane, title, items))
    if not choice then
        if selectError.code == "cancelled" then return nil end -- Escape
        return fail(ev, selectError.message)
    end
    local effects = open(ev, choice.id)
    -- Waited for, so that commands that do not change fail the run.
    local _, commandsError = afterglow.await(projectCommands(projects.names))
    if commandsError then
        table.insert(effects, afterglow.output_error(ev, "The open-NAME commands did not change: " .. commandsError.message .. "\n"))
        table.insert(effects, afterglow.exit(ev, 1))
    end
    return effects
end
return script
