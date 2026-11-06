-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Select and copy a retained shell prompt, input or output region.
-- From a shell, `aglow run semantic-regions copy` also needs Control.allowReadText.
-- A failure goes to stderr. A run from the palette shows it as a notification.
local function fail(ev, message) return {afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1)} end
return {
    metadata = {name="Shell regions", commands = { copy = { summary = "Copy a shell semantic region", palette = "Copy a shell semantic region" } }},
    run_command = function(_, ev)
        if #ev.args > 0 then
            return {afterglow.output_error(ev, "usage: aglow run semantic-regions copy\n"), afterglow.exit(ev, 2)}
        end
        if not ev.pane then return fail(ev, "There is no pane to read.") end
        local pane = ev.pane.id
        local found, err = afterglow.await(afterglow.semantic_regions(pane))
        if not found then return fail(ev, err.message) end
        local choices, regions = {}, found.regions or {}
        for i = #regions, 1, -1 do
            local region = regions[i]
            -- From a shell, the open output region is that of `aglow run` itself.
            -- A run does not tell whether --pane named another pane, so the open
            -- output region of that pane is left out too.
            local own = region.open and region.kind == "output" and ev.source == "shell"
            if region.available and not own then
                table.insert(choices, {id=region.id, title=region.kind.." #"..region.id
                    ..(region.open and " (open)" or "")..(region.start_trimmed and " (trimmed)" or "")})
                if #choices == 200 then break end
            end
        end
        if #choices == 0 then return fail(ev, "No retained shell markers in this pane.") end
        local choice, selectError = afterglow.await(afterglow.select(pane, "Copy shell region", choices))
        if not choice then
            if selectError.code == "cancelled" then return nil end
            return fail(ev, selectError.message)
        end
        local region, readError = afterglow.await(afterglow.read_region(pane, choice.id, 262144))
        if not region then return fail(ev, readError.message) end
        if not region.available then return fail(ev, "The region expired.") end
        -- A warning, not a failure: the copy happened, and the exit status stays 0.
        if region.truncated then return {afterglow.copy(region.text), afterglow.output_error(ev, "Copied the first 256 KiB.\n")} end
        return afterglow.copy(region.text)
    end,
}
