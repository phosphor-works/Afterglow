-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copy into ~/.config/afterglow/scripts, then run "Split and title" in the palette.
local script = {}
script.metadata = {
    id = "create-and-title",
    name = "Create and title",
    actions = { { id = "split", title = "Split and title" } },
}

function script.run_action(action, ev)
    if action == "split" and ev.pane then
        return afterglow.request("workspace", afterglow.split(ev.pane, "right"))
    end
end

function script.on_result(ev)
    if ev.request_id ~= "workspace" then return nil end
    if not ev.ok then return afterglow.log(ev.error.code .. ": " .. ev.error.message) end
    -- Creation is complete. Use the returned identity instead of predicting an
    -- ID or trying to associate a global on_pane_created event with this action.
    local pane = ev.data.pane
    if pane then return afterglow.set_title(pane, "Workspace") end
end

return script
