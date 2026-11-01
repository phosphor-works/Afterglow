-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local history = {}
-- State refuses object keys over 256 characters, so favorites and templates
-- keep only paths of at most 256 bytes. Recent entries are values, not keys.
history.MaxKey = 256
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
            if type(path) == "string" and #path <= history.MaxKey and pinned == true and count < 100 then
                result.favorites[path] = true; count += 1
            end
        end
    end
    if type(value.recent) == "table" then
        for _, item in ipairs(value.recent) do
            if entry(item) and #result.recent < 50 then table.insert(result.recent, {kind=item.kind, value=item.value}) end
        end
    end
    -- Templates only for listed directories: at most 150, so no cap drops the
    -- one just stored. A shell ("") is the default and needs no entry.
    local listed = table.clone(result.favorites)
    for _, item in ipairs(result.recent) do
        if item.kind == "directory" and #item.value <= history.MaxKey then listed[item.value] = true end
    end
    if type(value.templates) == "table" then
        for path, layout in pairs(value.templates) do
            if listed[path] and type(layout) == "string" and #layout > 0 and #layout <= 64 then
                result.templates[path] = layout
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
    if item.kind == "directory" and layout ~= nil then
        if layout == "" then value.templates[item.value] = nil else value.templates[item.value] = layout end
    end
    return history.normalize(value)
end
return history
