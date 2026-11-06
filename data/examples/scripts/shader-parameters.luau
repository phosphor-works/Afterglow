-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Example: Shaders.pane = ["bloom"] in config.json.
-- A failure goes to stderr. A run from the palette shows it as a notification.
local function fail(ev, message) return { afterglow.output_error(ev, message .. "\n"), afterglow.exit(ev, 1) } end
return {
    metadata = { name = "Live shader parameters", commands = {
        boost = { summary = "Boost Bloom in this tab or window", palette = "Boost Bloom in this tab or window" },
        reset = { summary = "Reset Bloom in this tab or window", palette = "Reset Bloom in this tab or window" },
    } },
    run_command = function(command, ev)
        if #ev.args > 0 then
            return { afterglow.output_error(ev, "usage: aglow run shader-parameters " .. command .. "\n"), afterglow.exit(ev, 2) }
        end
        if not ev.pane then return fail(ev, "There is no pane to change.") end
        local active, err = afterglow.await(afterglow.shaders(ev.pane))
        if not active then return fail(ev, err.message) end
        for _, shader in active.shaders or {} do
            if shader.shader == "bloom" then
                local scope = shader.layer == "window" and "window" or "tab"
                local values = command == "boost" and { strength = 1.2, radius = 10 } or {}
                return afterglow.shader_parameters(ev.pane, shader.instance, values, scope)
            end
        end
        return fail(ev, "Enable bloom in a shader chain first.")
    end,
}
