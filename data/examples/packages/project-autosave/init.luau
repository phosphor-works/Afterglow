-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local projects=require("projects")
local script={metadata={name="Project autosave",actions={
    {id="enable",title="Enable autosave for this saved project"},
    {id="disable",title="Disable autosave for this saved project"},
    {id="save",title="Save project now"},
    {id="restore",title="Restore an autosaved project"},
}}}
local saved, flow, cursor = {enabled={}}, nil, 0
local queue, saving, savingPane = {}, false, nil
local function notice(text,pane) return afterglow.notify("Project autosave",text,pane) end
local function request(id,effect) return afterglow.request(id,effect) end
function script.on_load(ev)
    saved=projects.normalize(ev.storage.value)
    local interval=math.floor(math.clamp(tonumber(ev.package.config.interval_seconds) or 60,10,3600))*1000
    return afterglow.timer("autosave",interval,interval)
end
local function next_save(state)
    while #queue>0 do
        local target=table.remove(queue,1)
        for _,w in ipairs(projects.candidates(state,saved.enabled)) do
            if w.id==target.id and w.project==target.project then
                saving=true;savingPane=w.active_pane
                return request("autosaved",afterglow.save_project(w.active_pane,w.project,true))
            end
        end
    end
    saving=false
end
function script.on_timer(ev)
    if ev.name~="autosave" or saving or flow then return nil end
    -- At most eight writes per interval; rotate fairly if there are more windows.
    local candidates=projects.candidates(ev.terminal,saved.enabled)
    if #candidates==0 then return nil end
    for i=1,math.min(8,#candidates) do
        cursor=cursor % #candidates+1
        table.insert(queue,candidates[cursor])
    end
    return next_save(ev.terminal)
end
function script.run_action(action,ev)
    if not ev.pane then return nil end
    if flow or saving then return notice("An autosave operation is already running.",ev.pane) end
    local name=projects.current(ev)
    if action~="restore" and not name then return notice("Use Save Project to name this window first.",ev.pane) end
    flow={action=action,pane=ev.pane.id,name=name}
    return request("state",afterglow.state_read())
end
function script.on_result(ev)
    if ev.request_id=="autosaved" then
        if not ev.ok then
            queue={};saving=false
            return notice("Autosave failed: "..ev.error.message,savingPane)
        end
        return next_save(ev.terminal)
    end
    if not flow then return nil end
    local function finish(message)
        local pane=flow.pane;flow=nil
        if message then return notice(message,pane) end
    end
    if not ev.ok then
        if ev.error.code=="cancelled" then return finish() end
        return finish(ev.error.message)
    end
    if ev.request_id=="state" then
        saved=projects.normalize(ev.data.value)
        if flow.action=="save" then return request("saved",afterglow.save_project(flow.pane,flow.name,true)) end
        if flow.action=="restore" then return request("listed",afterglow.list_projects()) end
        flow.value=projects.normalize(saved)
        if flow.action=="enable" then
            local count=0;for _ in pairs(flow.value.enabled) do count+=1 end
            if count>=100 and not saved.enabled[flow.name] then return finish("Autosave limit reached (100 projects).") end
            flow.value.enabled[flow.name]=true
        else flow.value.enabled[flow.name]=nil end
        return request("configured",afterglow.state_write(flow.value,ev.data.revision))
    elseif ev.request_id=="configured" then
        saved=projects.normalize(ev.data.value)
        if flow.action=="enable" then return request("saved",afterglow.save_project(flow.pane,flow.name,true)) end
        return finish("Autosave disabled for "..flow.name)
    elseif ev.request_id=="saved" then return finish("Saved "..flow.name)
    elseif ev.request_id=="listed" then
        local items={};flow.choices={}
        for _,name in ipairs(ev.data.names) do
            if saved.enabled[name] then
                local id=tostring(#items+1);flow.choices[id]=name
                table.insert(items,{id=id,title=name})
            end
        end
        if #items==0 then return finish("No autosaved projects. Save a project and enable autosave first.") end
        return request("chosen",afterglow.select(flow.pane,"Restore autosaved project",items))
    elseif ev.request_id=="chosen" then
        local name=flow.choices[ev.data.id]
        if not name then return finish() end
        return request("restored",afterglow.open_project(name))
    elseif ev.request_id=="restored" then return finish() end
end
return script
