-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copy to the active configuration's scripts/ directory.
-- A minimal project launcher using correlated results and the native selector.
local script = {}
script.metadata = {
    id = "projects",
    name = "Projects",
    actions = {{id="open", title="Choose saved project (Luau)"}},
}
local anchor = nil
function script.run_action(action, ev)
    if action == "open" and ev.pane then
        anchor = ev.pane.id
        return afterglow.request("names", afterglow.list_projects())
    end
end
function script.on_result(ev)
    if not ev.ok then return afterglow.log(ev.error.message) end
    if ev.request_id == "names" then
        local items = {}
        for _, name in ev.data.names do table.insert(items, {id=name, title=name}) end
        if #items == 0 then return afterglow.notify("Projects", "Save a project first using Save Project in the palette.", anchor) end
        return afterglow.request("choice", afterglow.select(anchor, "Open project", items))
    elseif ev.request_id == "choice" then
        return afterglow.request("opened", afterglow.open_project(ev.data.id))
    elseif ev.request_id == "opened" then
        return afterglow.log("Project window " .. ev.data.window_id .. (ev.data.reused and " focused" or " launched"))
    end
end
return script
