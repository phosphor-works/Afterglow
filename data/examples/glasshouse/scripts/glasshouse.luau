-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Glasshouse: turns what happens in the shell into the glass shaders'
-- signals. Needs the glasshouse shader packs and shell integration (OSC 133)
-- for the command events.
--
--   signal 0 strike  a command failed (value: its exit code)
--   signal 1 glaze   a long command, or a program's task, succeeded
--   signal 2 heat    1 while a command runs, 0 after
--   signal 3 ring    the bell
--   signal 4 status  a program's status (OSC 7501): 1 working, 2 blocked, 3 error

local STRIKE, GLAZE, HEAT, RING, STATUS = 0, 1, 2, 3, 4

-- A command this long that succeeds is glazed.
local GLAZE_AFTER_MS = 5000
-- One finishing out of sight after this long also sends a notification.
local NOTIFY_AFTER_MS = 20000
-- Exit codes that are not failures: Ctrl+C, and none reported.
local NOT_FAILED = { [130] = true, [-1] = true }

local STATUS_LEVEL = { working = 1, blocked = 2, error = 3 }

local script = {}

script.metadata = {
    id = "glasshouse",
    name = "Glasshouse",
    description = "Shell events drive the Glasshouse glass: cracks, glaze, heat and the bell",
    actions = {
        { id = "strike", title = "Glasshouse: strike the glass" },
        { id = "glaze", title = "Glasshouse: glaze the glass" },
        { id = "ring", title = "Glasshouse: ring the glass" },
        { id = "heat", title = "Glasshouse: heat the lamp until the next command" },
    },
}

function script.on_command_started(ev)
    return afterglow.signal(ev.pane, HEAT, 1)
end

function script.on_command_finished(ev)
    local effects = { afterglow.signal(ev.pane, HEAT, 0) }
    if ev.exit_code ~= 0 and not NOT_FAILED[ev.exit_code] then
        table.insert(effects, afterglow.signal(ev.pane, STRIKE, ev.exit_code))
    elseif ev.exit_code == 0 and ev.duration_ms >= GLAZE_AFTER_MS then
        table.insert(effects, afterglow.signal(ev.pane, GLAZE, ev.duration_ms / 1000))
    end
    if ev.duration_ms >= NOTIFY_AFTER_MS and not ev.pane.focused then
        local seconds = math.floor(ev.duration_ms / 1000)
        local outcome = ev.exit_code == 0 and "finished" or ("failed (" .. ev.exit_code .. ")")
        local what = ev.command ~= "" and ev.command or "A command"
        table.insert(effects, afterglow.notify(what, outcome .. " after " .. seconds .. " s"))
    end
    return effects
end

function script.on_bell(ev)
    return afterglow.signal(ev.pane, RING, 1)
end

function script.on_status(ev)
    local effects = { afterglow.signal(ev.pane, STATUS, STATUS_LEVEL[ev.state] or 0) }
    -- A program (an agent, a build) reporting its task done is glazed too.
    if ev.state == "done" then
        table.insert(effects, afterglow.signal(ev.pane, GLAZE, 1))
    end
    return effects
end

function script.run_action(action, ev)
    if not ev.pane then
        return nil
    end
    if action == "strike" then
        return afterglow.signal(ev.pane, STRIKE, 1)
    elseif action == "glaze" then
        return afterglow.signal(ev.pane, GLAZE, 1)
    elseif action == "ring" then
        return afterglow.signal(ev.pane, RING, 1)
    elseif action == "heat" then
        -- Warms as if a command were running; the next command cools it.
        return afterglow.signal(ev.pane, HEAT, 1)
    end
    return nil
end

return script
