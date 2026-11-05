-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local flow
-- This package's own overrides, by instance and tab (or window). The effective
-- values that the editor shows include other scripts' overrides, and sending
-- those would keep them in this package's layer after the other script stops.
local layers={}
local function finish(effect) flow=nil; return effect end
local function scope() return flow.shader.layer=="window" and "window" or "tab" end
local function layer()
    local key=flow.shader.instance.."|"..(scope()=="window" and "window" or tostring(flow.tab))
    if not layers[key] then layers[key]={window=flow.window,instance=flow.shader.instance,values={}} end
    return layers[key], key
end
-- Float parameters arrive widened from single precision: show 1.3, not 1.2999999523162842.
local function format(p,value)
    if p.type=="float" and type(value)=="number" then return string.format("%.7g",value) end
    return tostring(value)
end
-- Sends this package's layer with one parameter changed, because each update
-- replaces the layer. The layer keeps the value only when the host accepts it:
-- a refused value would otherwise go out again with every later edit.
local function edit(p,value)
    flow.pending=table.clone(layer().values)
    flow.pending[p.id]=value
    return afterglow.request("applied",afterglow.shader_parameters(flow.pane,flow.shader.instance,table.clone(flow.pending),scope(),{duration_ms=350}))
end
local function failure(ev,message,status)
    return {afterglow.output_error(ev,message.."\n"),afterglow.exit(ev,status or 1)}
end
-- The pane's shaders, or nil and an error effect.
local function shaders(ev)
    if not ev.pane then return nil,failure(ev,"There is no pane to read.") end
    local data,err=afterglow.await(afterglow.shaders(ev.pane))
    if not data then return nil,failure(ev,err.message) end
    return data.shaders
end
-- get [PARAM]: INSTANCE<TAB>PARAMETER<TAB>VALUE lines for the pane's shaders.
local function get(ev)
    if #ev.args>1 then return failure(ev,"usage: afterglow run scene-controls get [PARAM]",2) end
    local list,problem=shaders(ev)
    if not list then return problem end
    local lines={}
    for _,shader in ipairs(list) do
        for _,p in ipairs(shader.parameters) do
            if p.supported and (ev.args[1]==nil or ev.args[1]==p.id) then
                table.insert(lines,shader.instance.."\t"..p.id.."\t"..format(p,shader.values[p.id]))
            end
        end
    end
    if #lines==0 then return failure(ev,ev.args[1] and ("No parameter "..ev.args[1]) or "Enable a shader pack first.") end
    return afterglow.output(ev,table.concat(lines,"\n").."\n")
end
-- set PARAM VALUE: the first of the pane's shaders that has PARAM takes VALUE,
-- in this package's layer, as the editor sets it.
local function set(ev)
    if #ev.args~=2 then return failure(ev,"usage: afterglow run scene-controls set PARAM VALUE",2) end
    local list,problem=shaders(ev)
    if not list then return problem end
    local name,text=ev.args[1],ev.args[2]
    for _,shader in ipairs(list) do
        for _,p in ipairs(shader.parameters) do
            if p.supported and p.id==name then
                local value=text
                if p.type=="float" or p.type=="int" then
                    value=tonumber(text)
                    if not value then return failure(ev,name.." takes a number",2) end
                elseif p.type=="bool" then
                    if text~="true" and text~="false" then return failure(ev,name.." takes true or false",2) end
                    value=text=="true"
                end
                local window=shader.layer=="window"
                local key=shader.instance.."|"..(window and "window" or tostring(ev.pane.tab_id))
                local values=table.clone(layers[key] and layers[key].values or {})
                values[p.id]=value
                local applied,err=afterglow.await(afterglow.shader_parameters(ev.pane,shader.instance,table.clone(values),
                    window and "window" or "tab",{duration_ms=350}))
                if not applied then return failure(ev,err.message) end
                layers[key]={window=ev.pane.window,instance=shader.instance,values=values}
                return afterglow.output(ev,shader.instance.."\t"..p.id.."\t"..format(p,value).."\n")
            end
        end
    end
    return failure(ev,"No parameter "..name.." in the pane's shaders")
end
return {
 metadata={name="Scene controls"},
 run_command=function(command,ev)
    if command=="get" then return get(ev) end
    if command=="set" then return set(ev) end
    if not ev.pane then return nil end
    if flow then return afterglow.notify("Scene controls","Finish or cancel the open editor first.",ev.pane) end
    flow={pane=ev.pane.id,tab=ev.pane.tab_id,window=ev.pane.window}
    return afterglow.request("shaders",afterglow.shaders(flow.pane))
 end,
 on_result=function(ev)
    if not flow then return nil end
    if not ev.ok then
        if ev.error.code=="cancelled" then return finish(nil) end
        return finish(afterglow.notify("Scene controls",ev.error.message,flow.pane))
    end
    if ev.request_id=="shaders" then
        flow.shaders={}; local items={}; local present={}
        for _,shader in ipairs(ev.data.shaders) do
            present[shader.instance]=true
            local editable=false
            for _,p in ipairs(shader.parameters) do editable=editable or p.supported end
            if editable then
                flow.shaders[shader.instance]=shader
                table.insert(items,{id=shader.instance,title=shader.layer..": "..shader.shader})
            end
        end
        -- A configuration change replaces the instances and drops their overrides.
        for key,own in pairs(layers) do
            if own.window==flow.window and not present[own.instance] then layers[key]=nil end
        end
        if #items==0 then return finish(afterglow.notify("Scene controls","Enable a shader pack first.",flow.pane)) end
        return afterglow.request("shader",afterglow.select(flow.pane,"Select effect",items))
    elseif ev.request_id=="shader" then
        flow.shader=flow.shaders[ev.data.id]
        if not flow.shader then return finish(nil) end
        return afterglow.request("menu",afterglow.select(flow.pane,"Scene controls",{
            {id="presets",title="Choose preset"},{id="edit",title="Edit quality, intensity or another parameter"},
            {id="save",title="Save current values as preset"},{id="reset",title="Reset my changes to configured values"}}))
    elseif ev.request_id=="menu" then
        if ev.data.id=="presets" then return afterglow.request("presets",afterglow.shader_presets(flow.pane,flow.shader.instance)) end
        if ev.data.id=="save" then return afterglow.request("name",afterglow.prompt(flow.pane,"Preset name","My scene")) end
        if ev.data.id=="reset" then
            local _,key=layer(); layers[key]=nil
            return afterglow.request("applied",afterglow.shader_parameters(flow.pane,flow.shader.instance,{},scope(),{duration_ms=350}))
        end
        local items={}; flow.parameters={}
        for _,p in ipairs(flow.shader.parameters) do
            if p.supported then
                flow.parameters[p.id]=p
                table.insert(items,{id=p.id,title=p.name.." = "..format(p,flow.shader.values[p.id])})
            end
        end
        return afterglow.request("parameter",afterglow.select(flow.pane,"Edit parameter",items))
    elseif ev.request_id=="presets" then
        local items={}
        for _,p in ipairs(ev.data.presets) do table.insert(items,{id=p.id,title=p.name}) end
        if #items==0 then return finish(afterglow.notify("Scene controls","No presets for this effect.",flow.pane)) end
        return afterglow.request("preset",afterglow.select(flow.pane,"Choose preset",items))
    elseif ev.request_id=="preset" then
        flow.preset=true
        return afterglow.request("applied",afterglow.shader_preset(flow.pane,flow.shader.instance,ev.data.id,scope(),{duration_ms=350}))
    elseif ev.request_id=="name" then
        return afterglow.request("saved",afterglow.save_shader_preset(flow.pane,flow.shader.instance,ev.data.text))
    elseif ev.request_id=="parameter" then
        flow.parameter=flow.parameters[ev.data.id]
        local p=flow.parameter
        if not p then return finish(nil) end
        if p.type=="bool" then return edit(p,not flow.shader.values[p.id]) end
        local range=p.min and (" ["..format(p,p.min)..", "..(p.max and format(p,p.max) or "unbounded").."]") or ""
        return afterglow.request("value",afterglow.prompt(flow.pane,p.name..range,format(p,flow.shader.values[p.id])))
    elseif ev.request_id=="value" then
        local p=flow.parameter; local value=ev.data.text
        if p.type=="float" or p.type=="int" then
            value=tonumber(value)
            if not value then return finish(afterglow.notify("Scene controls","Enter a number.",flow.pane)) end
        end
        return edit(p,value)
    elseif ev.request_id=="applied" then
        -- A preset sets every supported parameter in this layer. Its own values
        -- come back in the result: the effective values also hold other scripts' overrides.
        if flow.preset then layer().values=table.clone(ev.data.values or {})
        elseif flow.pending then layer().values=flow.pending end
        return finish(nil)
    elseif ev.request_id=="saved" then return finish(nil) end
 end,
}
