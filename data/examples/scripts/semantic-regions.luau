-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Select and copy a retained shell prompt, input or output region.
local function notify(pane, text) return afterglow.notify("Shell regions", text, pane) end
return {
    metadata = {name="Shell regions", actions={{id="copy", title="Copy a shell semantic region"}}},
    run_action = function(_, ev)
        if not ev.pane then return nil end
        local pane = ev.pane.id
        local found, err = afterglow.await(afterglow.semantic_regions(pane))
        if not found then return notify(pane, err.message) end
        local choices = {}
        for i = #found.regions, 1, -1 do
            local region = found.regions[i]
            if region.available then
                table.insert(choices, {id=region.id, title=region.kind.." #"..region.id
                    ..(region.open and " (open)" or "")..(region.start_trimmed and " (trimmed)" or "")})
                if #choices == 200 then break end
            end
        end
        if #choices == 0 then return notify(pane, "No retained shell markers in this pane.") end
        local choice, selectError = afterglow.await(afterglow.select(pane, "Copy shell region", choices))
        if not choice then
            if selectError.code == "cancelled" then return nil end
            return notify(pane, selectError.message)
        end
        local region, readError = afterglow.await(afterglow.read_region(pane, choice.id, 262144))
        if not region then return notify(pane, readError.message) end
        if not region.available then return notify(pane, "The region expired.") end
        if region.truncated then return {afterglow.copy(region.text), notify(pane, "Copied the first 256 KiB.")} end
        return afterglow.copy(region.text)
    end,
}
