-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copy into ~/.config/afterglow/scripts, then run "Split and title" in the palette.
local script = {}
script.metadata = {
    id = "create-and-title",
    name = "Create and title",
    commands = { split = { summary = "Split and title", palette = "Split and title" } },
}

-- A failure goes to stderr. A run from the palette shows it as a notification.
local function fail(ev, message)
    return { afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1) }
end

function script.run_command(command, ev)
    if command ~= "split" then return nil end
    if #ev.args > 0 then
        return { afterglow.output_error(ev, "usage: afterglow run create-and-title split\n"), afterglow.exit(ev, 2) }
    end
    if not ev.pane then return fail(ev, "There is no pane to split.") end
    -- ev.cwd: the directory of `afterglow run`, or the pane's for the palette.
    local created, err = afterglow.await(afterglow.split(ev.pane, "right", nil,
        {size=0.35, cwd=ev.cwd, env={AFTERGLOW_WORKSPACE="1"}}))
    if not created then return fail(ev, err.code .. ": " .. err.message) end
    -- Creation is complete. Use the returned identity instead of predicting an
    -- ID or trying to associate a global on_pane_created event with this command.
    return afterglow.set_title(created.pane, "Workspace")
end

return script
