-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Install in ~/.config/afterglow/scripts/. Run "Inspect last build output"
-- from the command palette after a compiler command, or
-- `aglow run build-inspector inspect` in the pane. Requires shell marks.
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
    commands = { inspect = { summary = "Inspect last build output", palette = "Inspect last build output" } },
}

-- A failure goes to stderr. A run from the palette shows it as a notification.
local function fail(ev, message)
    return { afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1) }
end

-- The newest output region of the pane. From a shell, the open region is the
-- output of `aglow run` itself, so the newest finished one counts. A run
-- does not tell whether --pane named another pane, so the open region of that
-- pane is skipped too, also when it is the output of a command that still runs.
local function lastOutput(ev, regions)
    for i = #regions, 1, -1 do
        local region = regions[i]
        if region.kind == "output" and region.available and not (region.open and ev.source == "shell") then
            return region
        end
    end
    return nil
end

function script.run_command(command, ev)
    if command ~= "inspect" then return nil end
    if #ev.args > 0 then
        return { afterglow.output_error(ev, "usage: aglow run build-inspector inspect\n"), afterglow.exit(ev, 2) }
    end
    if not ev.pane then return fail(ev, "There is no pane to inspect.") end
    local pane = ev.pane
    -- Scripts.allowReadText can refuse the reads, and Control.allowReadText
    -- those of a run from the shell.
    local found, err = afterglow.await(afterglow.semantic_regions(pane))
    if not found then return fail(ev, err.message) end
    local region = lastOutput(ev, found.regions or {})
    if not region then return fail(ev, "No command-output marks in this pane.") end
    local output, readError = afterglow.await(afterglow.read_region(pane, region.id, 262144))
    if not output then return fail(ev, readError.message) end
    if not output.available then return fail(ev, "The command output has left the scrollback.") end
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
    if #items == 0 then return fail(ev, "No file:line diagnostics found.") end
    local choice, selectError = afterglow.await(afterglow.select(pane,
        "Build diagnostics" .. (output.truncated and " (output limited)" or ""), items))
    if not choice then
        if selectError.code == "cancelled" then return nil end -- Escape
        return fail(ev, selectError.message)
    end
    local diagnostic = choices[tonumber(choice.id)]
    if not diagnostic then return fail(ev, "The selection has expired.") end
    -- Explicit argv: compiler output never becomes shell code. Relative paths
    -- start from the directory that the run came from.
    return afterglow.new_tab(pane, ev.cwd, {"vi", "+" .. diagnostic.line, "--", diagnostic.path})
end
return script
