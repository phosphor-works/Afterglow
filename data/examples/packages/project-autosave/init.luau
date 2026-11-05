-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local projects=require("projects")
local script={metadata={name="Project autosave"}}
local saved, busy, cursor = {enabled={}}, false, 0
local queue, saving, savingPane = {}, false, nil
local function notice(text,pane) return afterglow.notify("Project autosave",text,pane) end
-- A command's message: on stdout or stderr from the shell, else a notification.
local function report(ev,text,failed)
    if ev.source=="shell" then
        if failed then return {afterglow.output_error(ev,text.."\n"),afterglow.exit(ev,1)} end
        return afterglow.output(ev,text.."\n")
    end
    return notice(text,ev.pane and ev.pane.id)
end
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
                return afterglow.request("autosaved",afterglow.save_project(w.active_pane,w.project,true))
            end
        end
    end
    saving=false
end
function script.on_timer(ev)
    if ev.name~="autosave" or saving or busy then return nil end
    -- At most eight writes per interval; rotate fairly if there are more windows.
    local candidates=projects.candidates(ev.terminal,saved.enabled)
    if #candidates==0 then return nil end
    for i=1,math.min(8,#candidates) do
        cursor=cursor % #candidates+1
        table.insert(queue,candidates[cursor])
    end
    return next_save(ev.terminal)
end
-- Saves the window's project now, under NAME when given.
local function save(ev)
    if #ev.args>1 then
        return {afterglow.output_error(ev,"usage: afterglow run project-autosave save [NAME]\n"),afterglow.exit(ev,2)}
    end
    if not ev.pane then return report(ev,"There is no pane to save.",true) end
    local name=ev.args[1] or projects.current(ev)
    if not name then return report(ev,"Name the project, or use Save Project first.",true) end
    local done,err=afterglow.await(afterglow.save_project(ev.pane.id,name,true))
    if not done then return report(ev,err.message,true) end
    return report(ev,"Saved "..name)
end
local function restore(ev)
    if not ev.pane then return report(ev,"There is no pane to restore in.",true) end
    local listed,err=afterglow.await(afterglow.list_projects())
    if not listed then return report(ev,err.message,true) end
    local items,choices={},{}
    for _,name in ipairs(listed.names) do
        if saved.enabled[name] then
            local id=tostring(#items+1);choices[id]=name
            table.insert(items,{id=id,title=name})
        end
    end
    if #items==0 then return report(ev,"No autosaved projects. Save a project and enable autosave first.",true) end
    local choice,problem=afterglow.await(afterglow.select(ev.pane.id,"Restore autosaved project",items))
    if not choice then
        if problem and problem.code~="cancelled" then return report(ev,problem.message,true) end
        return nil
    end
    if not choices[choice.id] then return nil end
    local opened,failure=afterglow.await(afterglow.open_project(choices[choice.id]))
    if not opened then return report(ev,failure.message,true) end
    return nil
end
-- Turns autosave on or off for the window's project, and saves it once when on.
local function configure(command,ev)
    local name=projects.current(ev)
    if not name then return report(ev,"Use Save Project to name this window first.",true) end
    local stored,err=afterglow.await(afterglow.state_read())
    if not stored then return report(ev,err.message,true) end
    local value=projects.normalize(stored.value)
    if command=="enable" then
        local count=0;for _ in pairs(value.enabled) do count+=1 end
        if count>=100 and not value.enabled[name] then return report(ev,"Autosave limit reached (100 projects).",true) end
        value.enabled[name]=true
    else value.enabled[name]=nil end
    local written,failure=afterglow.await(afterglow.state_write(value,stored.revision))
    if not written then return report(ev,failure.message,true) end
    saved=projects.normalize(written.value)
    if command=="disable" then return report(ev,"Autosave disabled for "..name) end
    return save(ev)
end
local function run(command,ev)
    if command=="save" then return save(ev) end
    if command=="restore" then return restore(ev) end
    return configure(command,ev)
end
function script.run_command(command,ev)
    -- One operation at a time, and none while the timer saves.
    if busy or saving then return report(ev,"An autosave operation is already running.",true) end
    busy=true
    local ok,effects=pcall(run,command,ev)
    busy=false
    if not ok then error(effects,0) end
    return effects
end
function script.on_result(ev)
    if ev.request_id~="autosaved" then return nil end
    if not ev.ok then
        queue={};saving=false
        return notice("Autosave failed: "..ev.error.message,savingPane)
    end
    return next_save(ev.terminal)
end
return script
