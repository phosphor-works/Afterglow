-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local flow
local function request(id, effect) return afterglow.request(id, effect) end
local function finish(effect) flow=nil; return effect end
local function scope() return flow.shader.layer=="window" and "window" or "tab" end
local function apply(values)
    return request("applied",afterglow.shader_parameters(flow.pane,flow.shader.instance,values,scope(),{duration_ms=350}))
end
return {
 metadata={name="Scene controls",actions={{id="open",title="Edit scene parameters and presets"}}},
 run_action=function(_,ev)
    if not ev.pane then return nil end
    if flow then return afterglow.notify("Scene controls","Finish or cancel the open editor first.",ev.pane) end
    flow={pane=ev.pane.id}
    return request("shaders",afterglow.shaders(flow.pane))
 end,
 on_result=function(ev)
    if not flow then return nil end
    if not ev.ok then
        local pane=flow.pane
        return finish(ev.error.code=="cancelled" and nil or afterglow.notify("Scene controls",ev.error.message,pane))
    end
    if ev.request_id=="shaders" then
        flow.shaders={}; local items={}
        for _,shader in ipairs(ev.data.shaders) do
            if #shader.parameters>0 then
                flow.shaders[shader.instance]=shader
                table.insert(items,{id=shader.instance,title=shader.layer..": "..shader.shader})
            end
        end
        if #items==0 then return finish(afterglow.notify("Scene controls","Enable a shader pack first.",flow.pane)) end
        return request("shader",afterglow.select(flow.pane,"Select effect",items))
    elseif ev.request_id=="shader" then
        flow.shader=flow.shaders[ev.data.id]
        return request("menu",afterglow.select(flow.pane,"Scene controls",{
            {id="presets",title="Choose preset"},{id="edit",title="Edit quality, intensity or another parameter"},
            {id="save",title="Save current values as preset"},{id="reset",title="Reset my changes to configured values"}}))
    elseif ev.request_id=="menu" then
        if ev.data.id=="presets" then return request("presets",afterglow.shader_presets(flow.pane,flow.shader.instance)) end
        if ev.data.id=="save" then return request("name",afterglow.prompt(flow.pane,"Preset name","My scene")) end
        if ev.data.id=="reset" then return apply({}) end
        local items={}; flow.parameters={}
        for _,p in ipairs(flow.shader.parameters) do
            if p.supported then
                flow.parameters[p.id]=p
                table.insert(items,{id=p.id,title=p.name.." = "..tostring(flow.shader.values[p.id])})
            end
        end
        return request("parameter",afterglow.select(flow.pane,"Edit parameter",items))
    elseif ev.request_id=="presets" then
        local items={}
        for _,p in ipairs(ev.data.presets) do table.insert(items,{id=p.id,title=p.name}) end
        return request("preset",afterglow.select(flow.pane,"Choose preset",items))
    elseif ev.request_id=="preset" then
        return request("applied",afterglow.shader_preset(flow.pane,flow.shader.instance,ev.data.id,scope(),{duration_ms=350}))
    elseif ev.request_id=="name" then
        return request("saved",afterglow.save_shader_preset(flow.pane,flow.shader.instance,ev.data.text))
    elseif ev.request_id=="parameter" then
        flow.parameter=flow.parameters[ev.data.id]
        local p=flow.parameter
        if p.type=="bool" then
            flow.shader.values[p.id]=not flow.shader.values[p.id]
            return apply(flow.shader.values)
        end
        local range=p.min and (" ["..p.min..", "..tostring(p.max or "unbounded").."]") or ""
        return request("value",afterglow.prompt(flow.pane,p.name..range,tostring(flow.shader.values[p.id])))
    elseif ev.request_id=="value" then
        local p=flow.parameter; local value=ev.data.text
        if p.type=="float" or p.type=="int" then
            value=tonumber(value)
            if not value then return finish(afterglow.notify("Scene controls","Enter a number.",flow.pane)) end
        end
        flow.shader.values[p.id]=value
        return apply(flow.shader.values)
    elseif ev.request_id=="applied" or ev.request_id=="saved" then return finish(nil) end
 end,
}
