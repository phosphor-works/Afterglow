-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- A package that ships a script and the shader it drives. Copy this folder
-- into ~/.config/afterglow/scripts, and run "Use Package Shaders…" (or
-- `aglow run scripted-overview use-shaders`). The dialog lists the
-- package's suggestion, Shaders.overview = scripted-overview.wall, and
-- writes it when you choose it. Then bind "script:scripted-overview.open".
-- The script owns the keyboard: a/d, h/l or the arrow keys move, w/s jump
-- between tabs, Enter or Space chooses, q or Escape closes. The key mode
-- starts while the overview is open, so it ends when the overview closes,
-- also when a click closes it.
local script = {}
script.metadata = { name = "Scripted overview" }

-- The window that holds the pane in the snapshot.
local function windowOf(terminal, pane)
    local id = nil
    for _, p in terminal.panes do
        if pane and p.id == pane.id then id = p.window end
    end
    for _, window in terminal.windows do
        if window.id == id then return window end
    end
    return nil
end

-- A failure goes to stderr. A run from the palette or a key shows it as a notification.
local function fail(ev, message)
    return { afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1) }
end

function script.run_command(command, ev)
    if command ~= "open" and command ~= "use-shaders" then return nil end
    if #ev.args > 0 then
        return { afterglow.output_error(ev, "usage: aglow run scripted-overview " .. command .. "\n"), afterglow.exit(ev, 2) }
    end
    if not ev.pane then return fail(ev, "There is no pane to act on.") end
    if command == "use-shaders" then
        -- Another dialog in the window refuses it.
        local shown, err = afterglow.await(afterglow.action(ev.pane, "use_package_shaders"))
        if not shown then return fail(ev, err.message) end
        return nil
    end
    local opened, err, result = afterglow.await(afterglow.overview(ev.pane, "open"))
    if not opened then return fail(ev, err.message) end
    -- A click can close the overview before its result arrives. A key mode
    -- that started then would not end with the overview.
    local window = windowOf(result.terminal, ev.pane)
    if not window or not window.overview.open then return nil end
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
    local window = windowOf(ev.terminal, ev.pane)
    -- The key mode belongs to the overview: without one, give the keyboard back.
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
    if key == "Enter" or key == "Space" then return afterglow.overview(ev.pane, "choose") end
    if key == "q" or key == "Escape" then return afterglow.overview(ev.pane, "close") end
    return nil
end

return script
