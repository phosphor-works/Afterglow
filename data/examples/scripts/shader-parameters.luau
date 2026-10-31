-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Example: Shaders.pane = ["bloom"] in config.json.
return {
    metadata = { name = "Live shader parameters", actions = {
        { id = "boost", title = "Boost Bloom in current tab" },
        { id = "reset", title = "Reset Bloom in current tab" },
    } },
    run_action = function(action, ev)
        if not ev.pane then return nil end
        local active, err = afterglow.await(afterglow.shaders(ev.pane))
        if not active then return afterglow.notify("Shader parameters", err.message, ev.pane) end
        for _, shader in active.shaders or {} do
            if shader.shader == "bloom" then
                local scope = shader.layer == "window" and "window" or "tab"
                local values = action == "boost" and { strength = 1.2, radius = 10 } or {}
                return afterglow.shader_parameters(ev.pane, shader.instance, values, scope)
            end
        end
        return afterglow.notify("Shader parameters", "Enable bloom in a shader chain first.", ev.pane)
    end,
}
