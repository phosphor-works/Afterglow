-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- A package that ships a script and the shader it drives. Copy this folder
-- into ~/.config/afterglow/scripts, run "Use Package Shaders…" to set
-- Shaders.overview to scripted-overview.wall, and bind
-- "script:scripted-overview.open". The script owns the keyboard: a/d, h/l or
-- the arrow keys move, w/s jump between tabs, Enter or Space chooses, q or
-- Escape closes.
local script = {}
script.metadata = {
    name = "Scripted overview",
    actions = { { id = "open", title = "Pane overview (scripted keys)" } },
}

local function windowOf(ev)
    for _, window in ev.terminal.windows do
        if ev.pane and window.id == ev.pane.window then return window end
    end
    return nil
end

function script.run_action(_, ev)
    if not ev.pane then return nil end
    local opened, err = afterglow.await(afterglow.overview(ev.pane, "open"))
    if not opened then return afterglow.notify("Scripted overview", err.message, ev.pane) end
    return afterglow.key_mode(ev.pane, "Panes · a/d move · w/s tabs · Enter choose · q close")
end

-- The first card of the next (step 1) or previous (step -1) tab.
local function tabJump(ev, window, step)
    local tabOf = {}
    for _, pane in ev.terminal.panes do tabOf[pane.id] = pane.tab_id end
    local cards = window.overview.cards
    local here = 1
    for i, id in cards do
        if id == window.overview.highlighted then here = i end
    end
    for k = 1, #cards do
        local i = (here - 1 + step * k) % #cards + 1
        if tabOf[cards[i]] ~= tabOf[cards[here]] then
            while i > 1 and tabOf[cards[i - 1]] == tabOf[cards[i]] do i -= 1 end
            return cards[i]
        end
    end
    return nil
end

function script.on_key(ev)
    local window = windowOf(ev)
    -- A click can close the overview without this script: give the keys back.
    if not window or not window.overview.open then return afterglow.end_key_mode() end
    -- Letters arrive as typed: "A" with Caps Lock or Shift. Named keys keep their case.
    local key = #ev.key == 1 and string.lower(ev.key) or ev.key
    if key == "a" or key == "h" or key == "Left" then return afterglow.overview_move(ev.pane, -1) end
    if key == "d" or key == "l" or key == "Right" then return afterglow.overview_move(ev.pane, 1) end
    if key == "w" or key == "s" then
        local target = tabJump(ev, window, key == "s" and 1 or -1)
        if target then return afterglow.overview_move(ev.pane, 0, target) end
        return nil
    end
    if key == "Enter" or key == "Space" then return { afterglow.overview(ev.pane, "choose"), afterglow.end_key_mode() } end
    if key == "q" or key == "Escape" then return { afterglow.overview(ev.pane, "close"), afterglow.end_key_mode() } end
    return nil
end

return script
