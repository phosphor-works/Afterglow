-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local history = {}
local function entry(value)
    return type(value) == "table" and (value.kind == "directory" or value.kind == "project")
        and type(value.value) == "string" and #value.value > 0 and #value.value <= 4096
end
function history.normalize(value)
    value = type(value) == "table" and value or {}
    local result = {favorites={}, recent={}, templates={}}
    local count = 0
    if type(value.favorites) == "table" then
        for path, pinned in pairs(value.favorites) do
            if type(path) == "string" and #path <= 4096 and pinned == true and count < 100 then
                result.favorites[path] = true; count += 1
            end
        end
    end
    if type(value.recent) == "table" then
        for _, item in ipairs(value.recent) do
            if entry(item) and #result.recent < 50 then table.insert(result.recent, {kind=item.kind, value=item.value}) end
        end
    end
    count = 0
    if type(value.templates) == "table" then
        for path, layout in pairs(value.templates) do
            if type(path) == "string" and #path <= 4096 and type(layout) == "string" and #layout <= 64 and count < 100 then
                result.templates[path]=layout; count+=1
            end
        end
    end
    return result
end
function history.migrate(value, from, to)
    assert(from == 1 and to == 2, "Unsupported navigator state migration")
    local converted = {favorites={},recent={},templates={}}
    for _, path in ipairs(value.favorites or {}) do
        if type(path) == "string" then converted.favorites[path]=true end
    end
    for _, path in ipairs(value.recent or {}) do
        if type(path) == "string" then table.insert(converted.recent,{kind="directory",value=path}) end
    end
    return history.normalize(converted)
end
function history.remember(value, item, layout)
    local recent = {{kind=item.kind, value=item.value}}
    for _, old in ipairs(value.recent) do
        if (old.kind ~= item.kind or old.value ~= item.value) and #recent < 50 then table.insert(recent,old) end
    end
    value.recent=recent
    if item.kind == "directory" and layout ~= nil then value.templates[item.value]=layout end
    return history.normalize(value)
end
return history
