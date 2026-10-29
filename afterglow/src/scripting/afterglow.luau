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
    log = function(message)
        return { kind = "log", message = tostring(message) }
    end,
}
