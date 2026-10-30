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
        return afterglow.request(action, afterglow.shaders(ev.pane))
    end,
    on_result = function(ev)
        if not ev.ok then
            return afterglow.notify("Shader parameters", ev.error.message, ev.pane)
        end
        if ev.request_id ~= "boost" and ev.request_id ~= "reset" then return nil end
        for _, shader in ev.data.shaders or {} do
            if shader.shader == "bloom" then
                local scope = shader.layer == "window" and "window" or "tab"
                local values = ev.request_id == "boost" and { strength = 1.2, radius = 10 } or {}
                return afterglow.request("updated", afterglow.shader_parameters(ev.pane, shader.instance, values, scope))
            end
        end
        return afterglow.notify("Shader parameters", "Enable bloom in a shader chain first.", ev.pane)
    end,
}
