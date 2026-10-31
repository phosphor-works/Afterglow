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
    if action ~= "split" or not ev.pane then return nil end
    local created, err = afterglow.await(afterglow.split(ev.pane, "right", nil,
        {size=0.35, cwd=ev.pane.cwd, env={AFTERGLOW_WORKSPACE="1"}}))
    if not created then return afterglow.log(err.code .. ": " .. err.message) end
    -- Creation is complete. Use the returned identity instead of predicting an
    -- ID or trying to associate a global on_pane_created event with this action.
    return afterglow.set_title(created.pane, "Workspace")
end

return script
