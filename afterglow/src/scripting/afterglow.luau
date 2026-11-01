-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The afterglow prelude: installed before scripts load, then frozen.
-- Handlers return effects built here; afterglow checks and applies them.
-- See afterglow.d.luau for the full API.

local function paneId(pane)
    if type(pane) == "table" then
        return pane.id
    end
    return pane
end

-- A missing value must fail the handler, not reach the shell or the clipboard as "nil".
local function required(value, helper, what)
    if value == nil then
        error(helper .. " needs " .. what, 3)
    end
    return tostring(value)
end

local function list(value)
    if value == nil then
        return {}
    end
    if type(value) == "string" then
        return { value }
    end
    return value
end

-- Await. Every handler runs as a coroutine. afterglow.await yields its effects
-- here; the handler returns them to afterglow tagged with a reserved request
-- ID, and their results resume the handler where it waited. Handlers that
-- never wait behave exactly as plain functions.
local AwaitPrefix = "afterglow.await:"
local MaxWaiting = 16 -- handlers waiting at once, per script
local MaxBatch = 16 -- effects in one await_all
-- Effects waited for at once, per script. Below the host's 64 pending results,
-- so waiting handlers alone cannot make it disable the script.
local MaxOutstanding = 48
local Yield = {} -- marks the runtime's own yields
local flows = {} -- waiting handlers by flow ID
local waiting = 0
local outstanding = 0
local nextFlow = 0
local current = nil -- the handler coroutine running now

local function checkEffect(effect, caller, level)
    if type(effect) ~= "table" or type(effect.kind) ~= "string" then
        error(caller, level)
    end
end

local function suspend(effects, many)
    if current == nil or coroutine.running() ~= current then
        error("afterglow.await works only in a handler, not at load time or inside another coroutine", 3)
    end
    return coroutine.yield(Yield, effects, many)
end

local function resumeIn(co, ...)
    local previous = current
    current = co
    local results = table.pack(coroutine.resume(co, ...))
    current = previous
    return results
end

-- Runs a handler coroutine until it finishes or waits. A finished handler
-- returns its effects; a waiting one returns the effects it waits for.
local function drive(co, ...)
    local r = resumeIn(co, ...)
    while true do
        if not r[1] then
            error(r[2], 0)
        end
        if coroutine.status(co) == "dead" then
            return r[2]
        end
        if r[2] ~= Yield then
            error("a handler can pause only through afterglow.await", 0)
        end
        local effects, many = r[3], r[4]
        local count = #effects
        if waiting < MaxWaiting and outstanding + count <= MaxOutstanding then
            local id = tostring(nextFlow + 1)
            local out = {}
            for i, effect in ipairs(effects) do
                local tagged = table.clone(effect)
                tagged.request_id = AwaitPrefix .. id .. ":" .. i
                out[i] = tagged
            end
            -- Registered only once the effects are built: a time limit inside
            -- the loop must not leave a flow that no result resumes.
            nextFlow += 1
            waiting += 1
            outstanding += count
            flows[id] = { co = co, many = many, count = count, remaining = count, results = {} }
            return out
        end
        -- A new table for each effect, so changing one result changes no other.
        local function busy(effect)
            return { ok = false, kind = effect.kind, request_id = "", data = {},
                error = { code = "busy", message = "Too many handlers are waiting" } }
        end
        if many then
            local all = {}
            for i, effect in ipairs(effects) do
                all[i] = busy(effect)
            end
            r = resumeIn(co, all)
        else
            local event = busy(effects[1])
            r = resumeIn(co, nil, event.error, event)
        end
    end
end

-- Afterglow calls this with each result whose request ID the runtime tagged.
local function resume(ev)
    local id, index = string.match(ev.request_id or "", "^afterglow%.await:(%d+):(%d+)$")
    local flow = id and flows[id]
    index = tonumber(index)
    -- A request ID the script made up must not complete a flow early.
    if not flow or index < 1 or index > flow.count or flow.results[index] ~= nil then
        return nil
    end
    flow.results[index] = ev
    flow.remaining -= 1
    outstanding -= 1
    if flow.remaining > 0 then
        return nil
    end
    flows[id] = nil
    waiting -= 1
    if flow.many then
        return drive(flow.co, flow.results)
    end
    return drive(flow.co, if ev.ok then ev.data else nil, ev.error, ev)
end

-- Afterglow calls this when a call that registered flows failed, for example
-- because its effects were too large to convert: none of those effects reached
-- the host, so no result will ever resume the flows after @since. A handler
-- that calls afterglow.__wrap itself can register more than one. Returns
-- whether it dropped any.
local function abandon(since)
    if type(since) ~= "number" then
        return false
    end
    local dropped = false
    -- The live flows, at most MaxWaiting: the range of ids since @since can
    -- be much longer, and this runs under the same time limit.
    for id, flow in pairs(flows) do
        if tonumber(id) > since then
            flows[id] = nil -- clearing a field that pairs visits is allowed
            waiting -= 1
            outstanding -= flow.remaining
            dropped = true
        end
    end
    return dropped
end

-- Handlers that must return at once: a migration returns state, and effects
-- returned while unloading are ignored.
local direct = { migrate = true, on_unload = true }

-- Afterglow loads every script through this: the module's functions run as
-- handler coroutines, and its other fields read through unchanged.
local function wrap(module)
    if type(module) ~= "table" then
        return module -- the engine refuses such a module; nothing to wrap
    end
    local cache = {}
    return setmetatable({}, {
        __index = function(_, key)
            if key == "__resume" then
                return resume
            elseif key == "__waiting" then
                return waiting
            elseif key == "__abandon" then
                return abandon
            elseif key == "__flows" then
                return nextFlow
            end
            -- rawget: the host's lookups must never run the script's own
            -- __index, which has no time limit outside a handler call.
            local value = rawget(module, key)
            if type(value) ~= "function" or direct[key] then
                return value
            end
            local entry = cache[key]
            if entry == nil or entry.handler ~= value then
                entry = { handler = value, call = function(...) return drive(coroutine.create(value), ...) end }
                cache[key] = entry
            end
            return entry.call
        end,
    })
end

afterglow = {
    __wrap = wrap,
    -- Waits for one effect. Returns its data, or nil and the error, then the result event.
    await = function(effect)
        checkEffect(effect, "afterglow.await expects an effect, such as afterglow.select(...)", 3)
        return suspend({ effect }, false)
    end,
    -- Waits for several effects, applied in order. Returns their result events in that order.
    await_all = function(effects)
        local usage = "afterglow.await_all expects a list of at most " .. MaxBatch .. " effects"
        if type(effects) ~= "table" or #effects > MaxBatch then
            error(usage, 2)
        end
        local count = 0
        for _, effect in ipairs(effects) do
            checkEffect(effect, usage, 3)
            count += 1
        end
        if count ~= #effects then
            error(usage .. ", with no holes", 2)
        end
        if count == 0 then
            return {}
        end
        return suspend(effects, true)
    end,
    -- Waits ms milliseconds, at most 600000.
    sleep = function(ms)
        if type(ms) ~= "number" then
            error("afterglow.sleep expects a number of milliseconds", 2)
        end
        return suspend({ { kind = "sleep", delay_ms = ms } }, false)
    end,
    open_directory = function(cwd, layout) return {kind="open_directory", cwd=cwd, layout=layout or ""} end,
    state_read = function() return {kind="state_read"} end,
    state_write = function(value, revision) return {kind="state_write", value=value, revision=revision} end,
    save_project = function(pane, name, overwrite)
        return {kind="save_project", pane=paneId(pane), name=name, overwrite=overwrite or false}
    end,
    open_project = function(name, newInstance)
        return {kind="open_project", name=name, new_instance=newInstance or false}
    end,
    list_projects = function() return {kind="list_projects"} end,
    move_pane = function(pane, target, placement)
        return {kind="move_pane", pane=paneId(pane), target=target and paneId(target) or 0, placement=placement or "right"}
    end,
    detach_tab = function(pane) return {kind="detach_tab", pane=paneId(pane)} end,
    timer = function(name, delay_ms, repeat_ms)
        if type(delay_ms) ~= "number" then
            error("afterglow.timer expects a delay in milliseconds", 2)
        end
        return {kind="timer", name=name, delay_ms=delay_ms, repeat_ms=repeat_ms or 0}
    end,
    cancel = function(name) return {kind="cancel", name=name} end,
    exec = function(name, command, options)
        local o = options or {}
        return {kind="exec", name=name, request_id=name, command=list(command), cwd=o.cwd or "",
            timeout_ms=o.timeout_ms or 30000, max_bytes=o.max_bytes or 65536, env=o.env}
    end,
    read_file = function(name, path, options)
        local o = options or {}
        return {kind="read_file", name=name, request_id=name, path=path,
            timeout_ms=o.timeout_ms or 30000, max_bytes=o.max_bytes or 65536}
    end,
    watch_file = function(name, path) return {kind="watch_file", name=name, path=path} end,
    set_actions = function(actions) return {kind="set_actions", actions=actions} end,
    select = function(pane, title, items) return {kind="select", pane=paneId(pane), title=title, items=items} end,
    prompt = function(pane, title, text) return {kind="prompt", pane=paneId(pane), title=title, text=text or ""} end,
    status = function(pane, text, tab_title) return {kind="status", pane=paneId(pane), text=text or "", tab_title=tab_title or ""} end,
    override = function(pane, values) return {kind="override", pane=paneId(pane), values=values} end,
    semantic_regions = function(pane) return {kind="semantic_regions", pane=paneId(pane)} end,
    read_region = function(pane, region, max_bytes)
        return {kind="read_region", pane=paneId(pane), region=region, max_bytes=max_bytes or 65536}
    end,
    read_text = function(pane, scope, max_bytes)
        return {kind="read_text", pane=paneId(pane), scope=scope or "last-output", max_bytes=max_bytes or 65536}
    end,
    -- Scope is relative to the anchor, or the globally active objects when
    -- omitted. This only reads the detached snapshot provided with the event.
    panes = function(state, scope, anchor)
        local out = {}
        local id = anchor == nil and state.active_pane or paneId(anchor)
        local tab, window = state.active_tab, state.active_window
        if anchor ~= nil then
            local found = false
            for _, pane in ipairs(state.panes) do
                if pane.id == id then tab, window, found = pane.tab_id, pane.window, true; break end
            end
            if not found then
                return out -- a stale anchor must not fall back to the active objects
            end
        end
        for _, pane in ipairs(state.panes) do
            if scope == nil or scope == "all" or (scope == "pane" and pane.id == id)
                or (scope == "tab" and pane.tab_id == tab) or (scope == "window" and pane.window == window) then
                table.insert(out, pane)
            end
        end
        return out
    end,
    request = function(id, effect)
        local out = table.clone(effect)
        out.request_id = id
        return out
    end,
    send_text = function(pane, text)
        return { kind = "send_text", pane = paneId(pane), text = required(text, "afterglow.send_text", "text") }
    end,
    notify = function(title, body, pane)
        return { kind = "notify", title = required(title, "afterglow.notify", "a title"), body = body and tostring(body) or "", pane = paneId(pane) }
    end,
    split = function(pane, direction, command, options)
        return { kind = "split", pane = paneId(pane), direction = direction or "right", command = list(command), options = options or {} }
    end,
    new_tab = function(pane, cwd, command, options)
        return { kind = "new_tab", pane = paneId(pane), cwd = cwd or "", command = list(command), options = options or {} }
    end,
    new_window = function(cwd, command, options)
        return { kind = "new_window", cwd = cwd or "", command = list(command), options = options or {} }
    end,
    focus = function(pane)
        return { kind = "focus", pane = paneId(pane) }
    end,
    close = function(pane)
        return { kind = "close", pane = paneId(pane) }
    end,
    set_title = function(pane, title)
        return { kind = "set_title", pane = paneId(pane), title = tostring(title or "") }
    end,
    copy = function(text)
        return { kind = "copy", text = required(text, "afterglow.copy", "text") }
    end,
    open_url = function(url)
        return { kind = "open_url", url = required(url, "afterglow.open_url", "a URL") }
    end,
    action = function(pane, name)
        return { kind = "action", pane = paneId(pane), name = required(name, "afterglow.action", "an action name") }
    end,
    scene_state = function(pane) return {kind="scene_state", pane=paneId(pane)} end,
    shader_presets = function(pane, instance) return {kind="shader_presets", pane=paneId(pane), instance=instance} end,
    shader_preset = function(pane, instance, preset, scope, transition)
        return {kind="shader_preset", pane=paneId(pane), instance=instance, preset=preset, scope=scope or "pane", transition=transition}
    end,
    save_shader_preset = function(pane, instance, name) return {kind="save_shader_preset", pane=paneId(pane), instance=instance, name=name} end,
    shaders = function(pane)
        return { kind = "shaders", pane = paneId(pane) }
    end,
    shader_parameters = function(pane, instance, values, scope, transition)
        return { kind = "shader_parameters", pane = paneId(pane), instance = instance, values = values, scope = scope or "pane", transition = transition }
    end,
    signal = function(pane, channel, value)
        return { kind = "signal", pane = paneId(pane), channel = channel, value = value or 1 }
    end,
    signals = function(pane, values, scope)
        return { kind = "signals", pane = paneId(pane), values = values, scope = scope or "tab" }
    end,
    log = function(message)
        return { kind = "log", message = tostring(message) }
    end,
    -- Sends the window's keys to on_key until end_key_mode. label shows in the window.
    key_mode = function(pane, label) return { kind = "key_mode", pane = paneId(pane), label = label or "" } end,
    end_key_mode = function() return { kind = "end_key_mode" } end,
    -- operation: "open", "close" (back to where it was) or "choose" (the highlighted pane).
    overview = function(pane, operation) return { kind = "overview", pane = paneId(pane), operation = operation or "open" } end,
    -- Moves the highlight to target, or by step cards.
    overview_move = function(pane, step, target)
        return { kind = "overview_move", pane = paneId(pane), step = step or 0, target = target and paneId(target) or 0 }
    end,
    -- effect: "portal", "flash", "shake", or a number a pack defines (4-255).
    pulse = function(pane, effect, options)
        local o = options or {}
        return { kind = "pulse", pane = paneId(pane), effect = tostring(effect), duration_ms = o.duration_ms or 600, color = o.color or 4 }
    end,
}
