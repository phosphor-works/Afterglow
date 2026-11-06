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
-- The arguments of each command, for its usage line. The handler cannot read
-- the manifest, so these repeat its usage fields.
local Usage={open="open",get="get [PARAM]",set="set PARAM VALUE"}
local Arguments={open={0,0},get={0,1},set={2,2}}
-- A shell change that waits for its result.
local busy=false
-- A decimal number, or nil. tonumber also reads hexadecimal, inf and nan,
-- which the host would refuse later as a failed change, not a usage error.
local function decimal(text)
    local mantissa=(string.gsub(text,"[eE][+-]?%d+$","",1))
    if not (string.match(mantissa,"^[+-]?%d+%.?%d*$") or string.match(mantissa,"^[+-]?%.%d+$")) then return nil end
    local value=tonumber(text)
    if value==nil or value~=value or value==math.huge or value==-math.huge then return nil end
    return value
end
-- The value that text gives the parameter, or nil and what the parameter takes.
local function parse(p,text)
    if p.type=="float" or p.type=="int" then
        local value=decimal(text)
        if value and p.type=="int" and value~=math.floor(value) then value=nil end
        if value==nil then return nil,p.type=="int" and "a whole number" or "a number" end
        return value
    elseif p.type=="bool" then
        if text~="true" and text~="false" then return nil,"true or false" end
        return text=="true"
    end
    return text
end
-- A configuration change replaces the instances and drops their overrides.
local function prune(window,shaders)
    local present={}
    for _,shader in ipairs(shaders) do present[shader.instance]=true end
    for key,own in pairs(layers) do
        if own.window==window and not present[own.instance] then layers[key]=nil end
    end
end
-- The pane's shaders and the pane as it is after the wait, or nil and an error effect.
local function shaders(ev)
    if not ev.pane then return nil,failure(ev,"There is no pane to read.") end
    local data,err,result=afterglow.await(afterglow.shaders(ev.pane))
    if not data then return nil,failure(ev,err.message) end
    for _,pane in ipairs(result.terminal.panes) do
        if pane.id==ev.pane.id then return data.shaders,nil,pane end
    end
    return nil,failure(ev,"The pane has closed.")
end
-- get [PARAM]: INSTANCE<TAB>PARAMETER<TAB>VALUE lines for the pane's shaders.
local function get(ev)
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
    local list,problem,pane=shaders(ev)
    if not list then return problem end
    prune(pane.window,list)
    local name,text=ev.args[1],ev.args[2]
    for _,shader in ipairs(list) do
        for _,p in ipairs(shader.parameters) do
            if p.supported and p.id==name then
                local value,takes=parse(p,text)
                if value==nil then return failure(ev,name.." takes "..takes,2) end
                local window=shader.layer=="window"
                local key=shader.instance.."|"..(window and "window" or tostring(pane.tab_id))
                local values=table.clone(layers[key] and layers[key].values or {})
                values[p.id]=value
                local applied,err=afterglow.await(afterglow.shader_parameters(pane.id,shader.instance,table.clone(values),
                    window and "window" or "tab",{duration_ms=350}))
                -- A run that ended during the wait has sent the change already.
                -- The layer keeps it, so that the next edit does not take it back.
                if applied or err.code=="cancelled" then layers[key]={window=pane.window,instance=shader.instance,values=values} end
                if not applied then return failure(ev,err.message) end
                return afterglow.output(ev,shader.instance.."\t"..p.id.."\t"..format(p,value).."\n")
            end
        end
    end
    return failure(ev,"No parameter "..name.." in the pane's shaders")
end
return {
 metadata={name="Scene controls"},
 run_command=function(command,ev)
    local range=Arguments[command]
    if not range then return nil end
    if #ev.args<range[1] or #ev.args>range[2] then return failure(ev,"usage: aglow run scene-controls "..Usage[command],2) end
    if command=="get" then return get(ev) end
    -- One change at a time: each change sends this package's whole layer, so
    -- two at once would each drop the value of the other.
    if busy or flow then return failure(ev,"Finish or cancel the open scene change first.") end
    if command=="set" then
        busy=true
        local ok,effects=pcall(set,ev)
        busy=false
        if not ok then error(effects,0) end
        return effects
    end
    if not ev.pane then return failure(ev,"There is no pane to edit.") end
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
        flow.shaders={}; local items={}
        for _,shader in ipairs(ev.data.shaders) do
            local editable=false
            for _,p in ipairs(shader.parameters) do editable=editable or p.supported end
            if editable then
                flow.shaders[shader.instance]=shader
                table.insert(items,{id=shader.instance,title=shader.layer..": "..shader.shader})
            end
        end
        prune(flow.window,ev.data.shaders)
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
        local p=flow.parameter
        local value,takes=parse(p,ev.data.text)
        if value==nil then return finish(afterglow.notify("Scene controls","Enter "..takes..".",flow.pane)) end
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
