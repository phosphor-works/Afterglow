-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local projects=require("projects")
local script={metadata={name="Project autosave"}}
local saved, busy, cursor = {enabled={}}, false, 0
local queue, saving, savingPane = {}, false, nil
-- The arguments of each command, for its usage line. The handler cannot read
-- the manifest, so these repeat its usage fields.
local Usage={save="save [NAME]",enable="enable",disable="disable",restore="restore"}
local function notice(text,pane) return afterglow.notify("Project autosave",text,pane) end
-- A failure goes to stderr and ends the run with status 1. A run with no
-- client (the palette, a key) shows it as a notification.
local function fail(ev,text) return {afterglow.output_error(ev,text.."\n"),afterglow.exit(ev,1)} end
-- A command's message: on stdout from the shell, else a notification on the pane.
local function report(ev,text)
    if ev.source=="shell" then return afterglow.output(ev,text.."\n") end
    if ev.pane then return notice(text,ev.pane.id) end
    return nil
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
    -- Only the saves of the last interval hold the next one back. A command
    -- that waits, such as an open restore picker, does not.
    if ev.name~="autosave" or saving then return nil end
    -- The enablement as stored: a command whose run ended while it wrote the
    -- enablement cannot know whether the write applied.
    local stored,_,result=afterglow.await(afterglow.state_read())
    if stored then saved=projects.normalize(stored.value) end
    -- The windows as they are after the wait. A refused wait has no snapshot.
    local state=result.terminal or ev.terminal
    -- At most eight writes per interval; rotate fairly if there are more windows.
    local candidates=projects.candidates(state,saved.enabled)
    if #candidates==0 then return nil end
    queue={}
    for i=1,math.min(8,#candidates) do
        cursor=cursor % #candidates+1
        table.insert(queue,candidates[cursor])
    end
    return next_save(state)
end
-- Saves the window's project now, under name when given. The window then has
-- that project name. Only the window's own project is replaced: a save under
-- the name of another saved project fails.
local function save(ev,name)
    if not ev.pane then return fail(ev,"There is no pane to save.") end
    local current=projects.current(ev)
    name=name or current
    if not name then return fail(ev,"Name the project, or use Save Project first.") end
    local replace=name==current
    local done,err=afterglow.await(afterglow.save_project(ev.pane.id,name,replace))
    if not done then
        if not replace then
            local listed=afterglow.await(afterglow.list_projects())
            if listed and table.find(listed.names,name) then
                return fail(ev,"A saved project "..name.." exists. Use Save Project in the palette to replace it.")
            end
        end
        return fail(ev,err.message)
    end
    return report(ev,"Saved "..name)
end
local function restore(ev)
    if not ev.pane then return fail(ev,"There is no pane to restore in.") end
    local listed,err=afterglow.await(afterglow.list_projects())
    if not listed then return fail(ev,err.message) end
    local items,choices={},{}
    for _,name in ipairs(listed.names) do
        if saved.enabled[name] then
            local id=tostring(#items+1);choices[id]=name
            table.insert(items,{id=id,title=name})
        end
    end
    if #items==0 then return fail(ev,"No autosaved projects. Save a project and enable autosave first.") end
    local choice,problem=afterglow.await(afterglow.select(ev.pane.id,"Restore autosaved project",items))
    if not choice then
        if problem and problem.code~="cancelled" then return fail(ev,problem.message) end
        return nil
    end
    if not choices[choice.id] then return fail(ev,"The selection has expired.") end
    local opened,failure=afterglow.await(afterglow.open_project(choices[choice.id]))
    if not opened then return fail(ev,failure.message) end
    return nil
end
-- Turns autosave on or off for the window's project. On, it also saves the
-- project once.
local function configure(command,ev)
    local name=projects.current(ev)
    if not name then return fail(ev,"Use Save Project to name this window first.") end
    local stored,err,result=afterglow.await(afterglow.state_read())
    if not stored then return fail(ev,err.message) end
    local value=projects.normalize(stored.value)
    local on=command=="enable"
    if (value.enabled[name]==true)~=on then
        if on then
            local count=0;for _ in pairs(value.enabled) do count+=1 end
            if count>=100 then return fail(ev,"Autosave limit reached (100 projects).") end
            value.enabled[name]=true
        else
            value.enabled[name]=nil
        end
        local written,failure
        written,failure,result=afterglow.await(afterglow.state_write(value,stored.revision))
        if not written then return fail(ev,failure.message) end
        value=projects.normalize(written.value)
    end
    saved=value
    if not on then return report(ev,"Autosave is off for "..name) end
    -- The window after the waits must still hold the project that was enabled.
    if projects.of(result.terminal or ev.terminal,ev.pane.id)~=name then
        return fail(ev,"Autosave is on for "..name..", but the window changed before the first save.")
    end
    local done,problem=afterglow.await(afterglow.save_project(ev.pane.id,name,true))
    if not done then return fail(ev,"Autosave is on for "..name..", but the first save failed: "..problem.message) end
    return report(ev,"Autosave is on for "..name..". Saved "..name)
end
function script.run_command(command,ev)
    if not Usage[command] then return nil end
    if #ev.args>(command=="save" and 1 or 0) then
        return {afterglow.output_error(ev,"usage: afterglow run project-autosave "..Usage[command].."\n"),afterglow.exit(ev,2)}
    end
    if command=="save" then return save(ev,ev.args[1]) end
    if command=="restore" then return restore(ev) end
    -- One change of the enablement at a time: each one reads and writes it.
    if busy then return fail(ev,"Autosave is being turned on or off already.") end
    busy=true
    local ok,effects=pcall(configure,command,ev)
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
