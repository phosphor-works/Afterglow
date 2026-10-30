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

local function list(value)
    if value == nil then
        return {}
    end
    if type(value) == "string" then
        return { value }
    end
    return value
end

afterglow = {
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
        return {kind="timer", name=name, delay_ms=delay_ms, repeat_ms=repeat_ms or 0}
    end,
    cancel = function(name) return {kind="cancel", name=name} end,
    exec = function(name, command, options)
        local o = options or {}
        return {kind="exec", name=name, request_id=name, command=list(command), cwd=o.cwd or "",
            timeout_ms=o.timeout_ms or 30000, max_bytes=o.max_bytes or 65536}
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
            tab, window = 0, 0 -- a stale anchor must not fall back to the active tab
            for _, pane in ipairs(state.panes) do
                if pane.id == id then tab, window = pane.tab_id, pane.window; break end
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
        return { kind = "send_text", pane = paneId(pane), text = tostring(text) }
    end,
    notify = function(title, body, pane)
        return { kind = "notify", title = tostring(title), body = body and tostring(body) or "", pane = paneId(pane) }
    end,
    split = function(pane, direction, command)
        return { kind = "split", pane = paneId(pane), direction = direction or "right", command = list(command) }
    end,
    new_tab = function(pane, cwd, command)
        return { kind = "new_tab", pane = paneId(pane), cwd = cwd or "", command = list(command) }
    end,
    new_window = function(cwd, command)
        return { kind = "new_window", cwd = cwd or "", command = list(command) }
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
        return { kind = "copy", text = tostring(text) }
    end,
    open_url = function(url)
        return { kind = "open_url", url = tostring(url) }
    end,
    action = function(pane, name)
        return { kind = "action", pane = paneId(pane), name = tostring(name) }
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
}
