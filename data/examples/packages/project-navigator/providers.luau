-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local providers = {}
function providers.path(path, home)
    if type(path) ~= "string" or #path > 4096 or string.find(path, "%z") then return nil end
    if path == "~" then path = home
    elseif string.sub(path, 1, 2) == "~/" then path = home .. string.sub(path, 2) end
    if string.sub(path, 1, 1) ~= "/" then return nil end
    path = string.gsub(path, "/+$", "")
    return path == "" and "/" or path
end
function providers.label(path)
    return string.match(path, "([^/]+)$") or path
end
function providers.jobs(config, home)
    local jobs, roots = {}, {}
    if type(config.roots) == "table" then
        for i, raw in ipairs(config.roots) do
            if i > 8 then break end
            local root = providers.path(raw, home)
            if root then
                table.insert(roots, root)
                if config.fd ~= false then
                    table.insert(jobs, {id="fd-"..i, kind="fd", command={"fd", "--hidden", "--no-ignore", "--max-depth",
                        tostring(math.floor(math.clamp(tonumber(config.max_depth) or 5, 1, 16))), "--glob", ".git", "--print0", "--", root}})
                end
            end
        end
    end
    if config.zoxide ~= false then table.insert(jobs, {id="zoxide", kind="zoxide", command={"zoxide", "query", "--list"}}) end
    return jobs, roots
end
function providers.parse(kind, output, home)
    local paths = {}
    local pattern = kind == "fd" and "([^%z]+)%z" or "([^\n]+)\n"
    -- Only complete records: a bounded/truncated result must not invent a path.
    for value in string.gmatch(output, pattern) do
        if #paths >= 400 then break end
        if kind == "fd" then value = string.gsub(value, "/?%.git/?$", "") end
        local path = providers.path(value, home)
        if path then table.insert(paths, path) end
    end
    return paths
end
return providers
