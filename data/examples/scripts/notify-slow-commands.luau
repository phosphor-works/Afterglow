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
    commands = { test = { summary = "Test the slow-command notification", palette = "Test the slow-command notification" } },
}

function script.on_command_finished(ev)
    if ev.duration_ms < 10000 or ev.pane.focused then
        return nil
    end
    local seconds = math.floor(ev.duration_ms / 1000)
    -- 130 is Ctrl+C, not a failure. -1: the shell reported no exit code, so
    -- the result is unknown.
    local outcome = ev.exit_code == 130 and "interrupted"
        or ev.exit_code > 0 and ("failed (" .. ev.exit_code .. ")") or "finished"
    return afterglow.notify(ev.command ~= "" and ev.command or "Command", outcome .. " after " .. seconds .. " s")
end

function script.run_command(command, ev)
    if command ~= "test" then return nil end
    if #ev.args > 0 then
        return { afterglow.output_error(ev, "usage: afterglow run notify-slow-commands test\n"), afterglow.exit(ev, 2) }
    end
    -- A notification belongs to a pane: without one, say so on stderr.
    if not ev.pane then
        return { afterglow.output_error(ev, "There is no pane to show the notification on.\n"), afterglow.exit(ev, 1) }
    end
    return afterglow.notify("Slow command", "This is what it will look like", ev.pane)
end

return script
