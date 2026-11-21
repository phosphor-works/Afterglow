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

-- Every pack that shows command heat takes it as a parameter, which eases
-- from the value on screen. Signal 2 still marks the change for other packs.
local HEATED = { ["phosphor-field"] = true, ["mesh-reactor"] = true, ["phosphor-frame"] = true }

-- The tab of each pane, as the pane events saw it: on_pane_moved tells only
-- where a pane went, and the tab that it left must cool.
local paneTab = {}

local function signals(pane, values)
    local channels = {}
    for _, value in ipairs(values) do
        table.insert(channels, { channel = value[1], value = value[2] })
    end
    return afterglow.signals(pane, channels, "tab")
end

-- The values go to the pane's tab. A heat value also goes to the packs that
-- show heat, whose instances on_result finds. The request names the pane:
-- a result has the pane of the event that sent it, which can have closed.
local function broadcast(pane, values)
    local effects = { signals(pane, values) }
    for _, value in ipairs(values) do
        if value[1] == 2 then
            local id = type(pane) == "table" and pane.id or pane
            table.insert(effects, afterglow.request("heat:" .. tostring(id) .. ":" .. tostring(value[2]), afterglow.shaders(pane)))
        end
    end
    return effects
end

-- The commandHeat transitions of the packs that show heat, in the pane's tab.
local function heatParameters(pane, value, shaders)
    local effects = {}
    for _, shader in ipairs(shaders or {}) do
        if HEATED[shader.shader] then
            table.insert(effects, afterglow.shader_parameters(pane, shader.instance, {commandHeat=value}, "tab",
                {duration_ms=650, curve="ease-in-out"}))
        end
    end
    return effects
end

local function heat(state, pane)
    for _, p in ipairs(afterglow.panes(state, "tab", pane)) do
        if p.command ~= "" then return 1 end
    end
    return 0
end

-- The heat of a tab again, through its active pane, while the tab exists.
local function reheat(state, tabId)
    for _, tab in ipairs(state.tabs) do
        if tab.id == tabId and tab.active_pane ~= 0 then
            return broadcast(tab.active_pane, { { 2, heat(state, tab.active_pane) } })
        end
    end
    return {}
end

function script.on_load(ev)
    for _, p in ipairs(ev.terminal.panes) do paneTab[p.id] = p.tab_id end
    return nil
end

function script.on_pane_created(ev)
    paneTab[ev.pane.id] = ev.pane.tab_id
    return broadcast(ev.pane, { { 2, heat(ev.terminal, ev.pane) } })
end

function script.on_pane_closed(ev)
    paneTab[ev.pane.id] = nil
    return reheat(ev.terminal, ev.pane.tab_id)
end

-- A pane that moves to another tab takes its shell command along: the tab
-- that it left and the tab that it joined both get their heat again. A tab
-- that moves keeps its panes, so it sends nothing.
function script.on_pane_moved(ev)
    local from = paneTab[ev.pane.id]
    paneTab[ev.pane.id] = ev.pane.tab_id
    if from == ev.pane.tab_id then return nil end
    local effects = from and reheat(ev.terminal, from) or {}
    for _, effect in ipairs(broadcast(ev.pane, { { 2, heat(ev.terminal, ev.pane) } })) do
        table.insert(effects, effect)
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

-- A failure goes to stderr. A run from the palette shows it as a notification.
local function fail(ev, message)
    return { afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1) }
end

-- Waits for the effects, so that one that Afterglow refuses fails the run.
-- Returns their results, or nil and the effects that fail the run.
local function apply(ev, effects)
    local results = afterglow.await_all(effects)
    for _, result in ipairs(results) do
        if not result.ok then return nil, fail(ev, result.error.message) end
    end
    return results
end

function script.run_command(command, ev)
    local step = STEPS[command]
    if not step then return nil end
    if #ev.args > 0 then
        return { afterglow.output_error(ev, "usage: aglow run phosphor " .. command .. "\n"), afterglow.exit(ev, 2) }
    end
    if not ev.pane then return fail(ev, "There is no pane to drive.") end
    -- What broadcast sends, waited for: the signals and, for heat, the packs that show it.
    local effects = { signals(ev.pane, { step }) }
    if step[1] == 2 then table.insert(effects, afterglow.shaders(ev.pane)) end
    local results, failure = apply(ev, effects)
    if not results or step[1] ~= 2 then return failure end
    local _, problem = apply(ev, heatParameters(ev.pane, step[2], (results[2].data or {}).shaders))
    return problem
end

-- The showcase drives the same steps through OSC 1337 user variables, so
-- that its script needs no fake build output to show the effects.
function script.on_user_var(ev)
    local step = ev.name == "phosphor" and STEPS[ev.value]
    if step then return broadcast(ev.pane, { step }) end
    return nil
end

-- The result of a heat request: its pane's tab eases to the heat.
function script.on_result(ev)
    if not ev.ok then return nil end
    local pane, value = string.match(ev.request_id, "^heat:(%d+):(%d+)$")
    if not pane then return nil end
    return heatParameters(tonumber(pane), tonumber(value), ev.data and ev.data.shaders)
end
return script
