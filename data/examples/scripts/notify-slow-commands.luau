-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Notifies when a command that ran for over ten seconds finishes in a pane
-- you are not looking at. Copy to ~/.config/afterglow/scripts/ to use it.
-- Needs shell integration (OSC 133), which fish and most prompts provide.

local script = {}

script.metadata = {
    id = "notify-slow-commands",
    name = "Notify on slow commands",
    description = "A desktop notification when a long command finishes out of sight",
    actions = { { id = "test", title = "Test the slow-command notification" } },
}

function script.on_command_finished(ev)
    if ev.duration_ms < 10000 or ev.pane.focused then
        return nil
    end
    local seconds = math.floor(ev.duration_ms / 1000)
    local outcome = ev.exit_code == 0 and "finished" or ("failed (" .. ev.exit_code .. ")")
    return afterglow.notify(ev.command ~= "" and ev.command or "Command", outcome .. " after " .. seconds .. " s")
end

function script.run_action(action, ev)
    if action == "test" then
        return afterglow.notify("Slow command", "This is what it will look like")
    end
    return nil
end

return script
