-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Install in ~/.config/afterglow/scripts/. Run "Inspect last build output"
-- from the command palette after a compiler command. Requires shell marks.
-- Opens selected file:line diagnostics in a new tab running vi.
local script = {}
-- Cuts text to at most max bytes without splitting a UTF-8 sequence.
local function trim(text, max)
    if #text <= max then return text end
    return string.sub(text, 1, utf8.offset(text, 0, max + 1) - 1)
end
script.metadata = {
    id = "build-inspector",
    name = "Build inspector",
    actions = {{ id = "inspect", title = "Inspect last build output" }},
}

function script.run_action(action, ev)
    if action ~= "inspect" or not ev.pane then return nil end
    local pane = ev.pane
    local output, err = afterglow.await(afterglow.read_text(pane, "last-output", 262144))
    -- Scripts.allowReadText can refuse the read: say so in the window.
    if not output then return afterglow.notify("Build inspector", err.message, pane) end
    if not output.available then
        return afterglow.notify("Build inspector", "No command-output marks in this pane.", pane)
    end
    local choices, items, seen = {}, {}, {}
    for line in string.gmatch(output.text, "[^\r\n]+") do
        local path, number = string.match(line, "^%s*(.-):(%d+):")
        if path and #path > 0 and #path < 400 and not string.find(path, "%c") then
            local key = path .. ":" .. number
            if not seen[key] then
                seen[key] = true
                table.insert(choices, {path=path, line=number})
                table.insert(items, {id=tostring(#choices), title=trim(line, 500)})
                if #choices == 200 then break end
            end
        end
    end
    if #items == 0 then
        return afterglow.notify("Build inspector", "No file:line diagnostics found.", pane)
    end
    local choice, selectError = afterglow.await(afterglow.select(pane,
        "Build diagnostics" .. (output.truncated and " (output limited)" or ""), items))
    if not choice then
        if selectError.code == "cancelled" then return nil end -- Escape
        return afterglow.notify("Build inspector", selectError.message, pane)
    end
    local diagnostic = choices[tonumber(choice.id)]
    if not diagnostic then return nil end
    -- Explicit argv: compiler output never becomes shell code.
    return afterglow.new_tab(pane, pane.cwd, {"vi", "+" .. diagnostic.line, "--", diagnostic.path})
end
return script
