-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- One material per tab. A scoped batch updates every pane with one clock.
local script = {}
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

local function broadcast(pane, values)
    local channels = {}
    for _, value in ipairs(values) do
        table.insert(channels, { channel = value[1], value = value[2] })
    end
    local signal = afterglow.signals(pane, channels, "tab")
    for _, value in ipairs(values) do
        if value[1] == 2 then
            return { signal, afterglow.request("heat:" .. tostring(value[2]), afterglow.shaders(pane)) }
        end
    end
    return signal
end

local function heat(state, pane)
    for _, p in ipairs(afterglow.panes(state, "tab", pane)) do
        if p.command ~= "" then return 1 end
    end
    return 0
end

function script.on_pane_created(ev)
    return broadcast(ev.pane, { { 2, heat(ev.terminal, ev.pane) } })
end

function script.on_pane_closed(ev)
    local effects = {}
    for _, tab in ipairs(ev.terminal.tabs) do
        if tab.active and tab.active_pane ~= 0 then
            for _, effect in ipairs(broadcast(tab.active_pane, { { 2, heat(ev.terminal, tab.active_pane) } })) do
                table.insert(effects, effect)
            end
        end
    end
    return effects
end

-- Focus does not change a tab material. Command and pane lifecycle events
-- already update their owning tab, including background tabs. Re-sending on
-- every focus event wastes the bounded effect budget and restarts transitions.

function script.on_command_started(ev)
    return broadcast(ev.pane, { { 2, 1 } })
end

function script.on_command_finished(ev)
    local values = { { 2, heat(ev.terminal, ev.pane) }, { 4, 0 } }
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
function script.on_result(ev)
    if not ev.ok or string.sub(ev.request_id, 1, 5) ~= "heat:" then return nil end
    local value = tonumber(string.sub(ev.request_id, 6))
    for _, shader in ipairs(ev.data.shaders or {}) do
        if shader.shader == "phosphor-field" then
            return afterglow.shader_parameters(ev.pane, shader.instance, {commandHeat=value}, "tab",
                {duration_ms=650, curve="ease-in-out"})
        end
    end
end
return script
