-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local projects = {}
function projects.normalize(value)
    local enabled, count = {}, 0
    for name, on in pairs(type(value.enabled)=="table" and value.enabled or {}) do
        if on==true and type(name)=="string" and #name<=64 and string.match(name,"^[%w_-]+$") and count<100 then
            enabled[name]=true; count+=1
        end
    end
    return {enabled=enabled}
end
function projects.current(ev)
    if not ev.pane then return nil end
    for _,w in ipairs(ev.terminal.windows) do
        if w.id==ev.pane.window and w.project~="" then return w.project end
    end
end
function projects.candidates(state, enabled)
    local counts, windows = {}, {}
    for _,w in ipairs(state.windows) do
        if w.project and w.project~="" then counts[w.project]=(counts[w.project] or 0)+1 end
    end
    for _,w in ipairs(state.windows) do
        -- Two instances must never race to replace the same saved layout.
        if enabled[w.project] and counts[w.project]==1 and w.active_pane~=0 then table.insert(windows,w) end
    end
    table.sort(windows,function(a,b) return a.project<b.project end)
    return windows
end
return projects
