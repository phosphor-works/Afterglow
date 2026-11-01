-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copy into ~/.config/afterglow/scripts and bind "script:overview-keys.open".
-- The pane overview, with this script owning the keyboard: a/d or h/l move,
-- w/s jump between tabs, Enter or Space chooses, q or Escape closes.
local script = {}
script.metadata = {
    id = "overview-keys",
    name = "Overview keys",
    actions = { { id = "open", title = "Pane overview (script keys)" } },
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
    if not opened then return afterglow.log(err.message) end
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
    local key = ev.key
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
