-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copy to the active configuration's scripts/ directory.
-- A minimal project launcher using awaited results and the native selector.
local script = {}
script.metadata = {
    id = "projects",
    name = "Projects",
    actions = {{id="open", title="Choose saved project (Luau)"}},
}
function script.run_action(action, ev)
    if action ~= "open" or not ev.pane then return nil end
    local projects, err = afterglow.await(afterglow.list_projects())
    if not projects then return afterglow.notify("Projects", err.message, ev.pane) end
    local items = {}
    for _, name in projects.names do table.insert(items, {id=name, title=name}) end
    if #items == 0 then
        return afterglow.notify("Projects", "Save a project first using Save Project in the palette.", ev.pane)
    end
    local choice, selectError = afterglow.await(afterglow.select(ev.pane, "Open project", items))
    if not choice then
        if selectError.code == "cancelled" then return nil end -- Escape
        return afterglow.notify("Projects", selectError.message, ev.pane)
    end
    local opened, openError = afterglow.await(afterglow.open_project(choice.id))
    if not opened then return afterglow.notify("Projects", openError.message, ev.pane) end
    return afterglow.log("Project window " .. opened.window_id .. (opened.reused and " focused" or " launched"))
end
return script
