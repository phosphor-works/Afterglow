-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local enabled, pending = false, false
local base, peak = 0.8, 1.3
local sample, visited = nil, {}
local function request(id,effect) return afterglow.request(id,effect) end
return {
 metadata={name="Contextual scene",actions={{id="toggle",title="Toggle contextual Phosphor exposure"}}},
 on_load=function(ev)
    base=math.clamp(tonumber(ev.package.config.base_intensity) or 0.8,0,3)
    peak=math.clamp(tonumber(ev.package.config.peak_intensity) or 1.3,base,3)
 end,
 run_action=function(_,ev)
    enabled=not enabled
    if enabled then return afterglow.timer("scene",1,1000) end
    local effects={afterglow.cancel("scene")}
    for window,target in pairs(visited) do
        for _,pane in ipairs(ev.terminal.panes) do
            if pane.window==window then
                table.insert(effects,afterglow.shader_parameters(pane,target.instance,{},"window")); break
            end
        end
    end
    visited={}; pending=false
    return effects
 end,
 on_timer=function(ev)
    if not enabled or pending or ev.terminal.active_pane==0 then return nil end
    pending=true
    return request("sample",afterglow.scene_state(ev.terminal.active_pane))
 end,
 on_result=function(ev)
    if not enabled then return nil end
    if not ev.ok then pending=false; return nil end
    if ev.request_id=="sample" then
        sample=ev.data.scene
        return request("shaders",afterglow.shaders(ev.pane))
    elseif ev.request_id=="shaders" then
        pending=false
        if ev.terminal.active_tab~=sample.tab_id then return nil end
        for _,shader in ipairs(ev.data.shaders) do
            if shader.shader=="phosphor-field" then
                local count=0; for _ in pairs(visited) do count+=1 end
                if not visited[ev.pane.window] and count>=8 then return nil end
                visited[ev.pane.window]={pane=ev.pane.id,instance=shader.instance}
                local drive=math.max(sample.history_normalized,sample.audio_available and sample.overall or 0)
                return afterglow.shader_parameters(ev.pane,shader.instance,{intensity=base+(peak-base)*drive},"tab",{duration_ms=900})
            end
        end
    end
 end,
}
