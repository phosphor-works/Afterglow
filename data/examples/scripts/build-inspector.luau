-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Install in ~/.config/afterglow/scripts/. Run "Inspect last build output"
-- from the command palette after a compiler command. Requires shell marks.
-- Opens selected file:line diagnostics in a new tab running vi.
local script = {}
local pending = {}
script.metadata = {
    id = "build-inspector",
    name = "Build inspector",
    actions = {{ id = "inspect", title = "Inspect last build output" }},
}

function script.run_action(action, ev)
    if action ~= "inspect" or not ev.pane then return nil end
    local id = "output-" .. ev.pane.id
    pending[id] = { pane = ev.pane }
    return afterglow.request(id, afterglow.read_text(ev.pane, "last-output", 262144))
end

function script.on_result(ev)
    local state = pending[ev.request_id]
    pending[ev.request_id] = nil
    if not state then return nil end
    if not ev.ok then
        return afterglow.log(ev.error.message)
    end
    if state.choices then
        local diagnostic = state.choices[tonumber(ev.data.id)]
        if not diagnostic then return nil end
        -- Explicit argv: compiler output never becomes shell code.
        return afterglow.new_tab(state.pane, state.pane.cwd,
            {"vi", "+" .. diagnostic.line, "--", diagnostic.path})
    end
    if not ev.data.available then
        return afterglow.notify("Build inspector", "No command-output marks in this pane.", state.pane)
    end
    local choices, items, seen = {}, {}, {}
    for line in string.gmatch(ev.data.text, "[^\r\n]+") do
        local path, number = string.match(line, "^%s*(.-):(%d+):")
        if path and #path > 0 and #path < 400 and not string.find(path, "%c") then
            local key = path .. ":" .. number
            if not seen[key] then
                seen[key] = true
                table.insert(choices, {path=path, line=number})
                table.insert(items, {id=tostring(#choices), title=string.sub(line,1,500)})
                if #choices == 200 then break end
            end
        end
    end
    if #items == 0 then
        return afterglow.notify("Build inspector", "No file:line diagnostics found.", state.pane)
    end
    local id = "choose-" .. state.pane.id
    pending[id] = {pane=state.pane, choices=choices}
    return afterglow.request(id, afterglow.select(state.pane,
        "Build diagnostics" .. (ev.data.truncated and " (output limited)" or ""), items))
end
function script.on_pane_closed(ev)
    for id, state in pairs(pending) do
        if state.pane.id == ev.pane.id then pending[id] = nil end
    end
end
return script
