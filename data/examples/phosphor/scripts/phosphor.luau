-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- One material per tab. A scoped batch updates every pane with one clock.
local script = {}
script.metadata = {
    id = "phosphor", name = "Phosphor",
    description = "Charge, fracture and recall a shared 3D containment core",
    commands = {
        strike = { summary = "Fracture the core", palette = "Phosphor: fracture the core" },
        complete = { summary = "Recall the fragments", palette = "Phosphor: recall the fragments" },
        ring = { summary = "Send a shock front", palette = "Phosphor: send a shock front" },
        heat = { summary = "Charge the core", palette = "Phosphor: charge the core" },
        cool = { summary = "Let the charged core cool" },
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
        if ev.pane and tab.id == ev.pane.tab_id and tab.active_pane ~= 0 then
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

-- The demonstration steps, by name: a channel and its value.
local STEPS = { strike = { 0, 1 }, complete = { 1, 1 }, ring = { 3, 1 }, heat = { 2, 1 }, cool = { 2, 0 } }

function script.run_command(command, ev)
    local step = STEPS[command]
    if not step then return nil end
    if #ev.args > 0 then
        return { afterglow.output_error(ev, "usage: aglow run phosphor " .. command .. "\n"), afterglow.exit(ev, 2) }
    end
    -- A failure goes to stderr. A run from the palette shows it as a notification.
    if not ev.pane then
        return { afterglow.output_error(ev, "There is no pane to drive.\n"), afterglow.exit(ev, 1) }
    end
    return broadcast(ev.pane, { step })
end

-- The showcase drives the same steps through OSC 1337 user variables, so
-- that its script needs no fake build output to show the effects.
function script.on_user_var(ev)
    local step = ev.name == "phosphor" and STEPS[ev.value]
    if step then return broadcast(ev.pane, { step }) end
    return nil
end
-- Every pack that shows command heat takes it as a parameter, which eases
-- from the value on screen. Signal 2 still marks the change for other packs.
local HEATED = { ["phosphor-field"] = true, ["mesh-reactor"] = true, ["phosphor-frame"] = true }
function script.on_result(ev)
    if not ev.ok or string.sub(ev.request_id, 1, 5) ~= "heat:" then return nil end
    local value = tonumber(string.sub(ev.request_id, 6))
    local effects = {}
    for _, shader in ipairs((ev.data and ev.data.shaders) or {}) do
        if HEATED[shader.shader] then
            table.insert(effects, afterglow.shader_parameters(ev.pane, shader.instance, {commandHeat=value}, "tab",
                {duration_ms=650, curve="ease-in-out"}))
        end
    end
    return effects
end
return script
