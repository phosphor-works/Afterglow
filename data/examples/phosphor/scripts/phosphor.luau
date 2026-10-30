-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- One material per window. Broadcast its events to the panes in that window;
-- the renderer already gives all of them the same clock and scene coordinates.
local script = {}
local panes, running = {}, {}
script.metadata = {
    id = "phosphor", name = "Phosphor",
    description = "Charge, fracture and recall a shared 3D containment core",
    actions = {
        { id = "strike", title = "Phosphor: fracture the core" },
        { id = "complete", title = "Phosphor: recall the fragments" },
        { id = "ring", title = "Phosphor: send a shock front" },
        { id = "heat", title = "Phosphor: charge the core" },
    },
}

local function remember(pane)
    panes[pane.id] = pane.window or 0
end

local function broadcast(pane, values)
    remember(pane)
    local ids, effects = {}, {}
    for id, window in pairs(panes) do
        if window == (pane.window or 0) then table.insert(ids, id) end
    end
    table.sort(ids)
    for _, id in ipairs(ids) do
        for _, value in ipairs(values) do
            table.insert(effects, afterglow.signal(id, value[1], value[2]))
        end
    end
    return effects
end

local function heat(window)
    for id in pairs(running) do
        if panes[id] == window then return 1 end
    end
    return 0
end

function script.on_pane_created(ev)
    remember(ev.pane)
    return afterglow.signal(ev.pane, 2, heat(ev.pane.window or 0))
end

function script.on_pane_closed(ev)
    local window = panes[ev.pane.id]
    panes[ev.pane.id], running[ev.pane.id] = nil, nil
    for id, w in pairs(panes) do
        if w == window then return broadcast({ id = id, window = w }, { { 2, heat(w) } }) end
    end
end

function script.on_command_started(ev)
    remember(ev.pane)
    running[ev.pane.id] = true
    return broadcast(ev.pane, { { 2, 1 } })
end

function script.on_command_finished(ev)
    running[ev.pane.id] = nil
    local values = { { 2, heat(ev.pane.window or 0) }, { 4, 0 } }
    if ev.exit_code ~= 0 and ev.exit_code ~= 130 and ev.exit_code ~= -1 then
        table.insert(values, { 0, ev.exit_code })
    elseif ev.exit_code == 0 and ev.duration_ms >= 1500 then
        table.insert(values, { 1, ev.duration_ms / 1000 })
    end
    return broadcast(ev.pane, values)
end

function script.on_bell(ev)
    return broadcast(ev.pane, { { 3, 1 } })
end

function script.on_status(ev)
    local states = { working = 1, blocked = 2, error = 3 }
    local values = { { 4, states[ev.state] or 0 } }
    if ev.state == "done" then table.insert(values, { 1, 1 })
    elseif ev.state == "error" then table.insert(values, { 0, 1 }) end
    return broadcast(ev.pane, values)
end

function script.run_action(action, ev)
    if not ev.pane then return nil end
    local channels = { strike = 0, complete = 1, ring = 3, heat = 2 }
    local channel = channels[action]
    if channel then return broadcast(ev.pane, { { channel, 1 } }) end
    if action == "cool" then return broadcast(ev.pane, { { 2, 0 } }) end
    return nil
end

-- The showcase drives the same script actions through OSC 1337 user vars.
-- There are no timers in Luau and no fake build output to explain the effects.
function script.on_user_var(ev)
    if ev.name == "phosphor" then return script.run_action(ev.value, ev) end
    return nil
end
return script
