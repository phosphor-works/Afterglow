-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local providers = require("providers")
local history = require("history")
local script = {}
script.metadata = {
    name="Project navigator",
    actions={
        {id="open", title="Navigate projects"},
        {id="favorite", title="Toggle favorite project directory"},
        {id="previous", title="Previous project"},
    },
}
local config, home, saved, revision
local flow = nil
script.migrate = history.migrate
function script.on_load(ev)
    config, home = ev.package.config, ev.package.home
    saved, revision = history.normalize(ev.storage.value), ev.storage.revision
end
local function notice(message, pane)
    return afterglow.notify("Project navigator", message, pane)
end
local function finish(message)
    local pane = flow and flow.pane
    flow = nil
    if message then return notice(message,pane) end
end
local function request(id, effect) return afterglow.request(id,effect) end
local function add(kind, value, running)
    if type(value) ~= "string" or value == "" then return end
    local key = kind .. ":" .. value
    if flow.entries[key] then
        if running then flow.entries[key].running=running end
    elseif flow.count < 400 then
        flow.count+=1; flow.entries[key]={kind=kind,value=value,running=running}
    end
end
local function picker()
    if flow.pending > 0 then return nil end
    local entries = {}
    for _, item in pairs(flow.entries) do
        local score = item.running and 100 or 0
        if item.kind == "directory" and saved.favorites[item.value] then score+=1000 end
        for i, recent in ipairs(saved.recent) do
            if recent.kind==item.kind and recent.value==item.value then score+=100-i; break end
        end
        item.score=score
        table.insert(entries,item)
    end
    table.sort(entries,function(a,b)
        if a.score~=b.score then return a.score>b.score end
        return a.value<b.value
    end)
    local items = {}
    flow.choices={}
    for i,item in ipairs(entries) do
        local id=tostring(i)
        flow.choices[id]=item
        local label=item.kind=="project" and ("Layout: "..item.value) or item.value
        if item.running then label="[running] "..label end
        if item.kind=="directory" and saved.favorites[item.value] then label="★ "..label end
        table.insert(items,{id=id,title=string.sub(label,1,500)})
    end
    if #items==0 then return finish("No projects found. Configure roots or save a project layout first.") end
    local title="Navigate projects"
    if #flow.failed>0 then title ..=" (unavailable: "..table.concat(flow.failed,", ")..")" end
    return request("choose",afterglow.select(flow.pane,title,items))
end
local function launch(item, layout)
    flow.selected=item; flow.layout=layout
    if item.kind=="project" then return request("opened",afterglow.open_project(item.value)) end
    return request("opened",afterglow.open_directory(item.value,layout))
end
local function choose(item)
    if item.kind=="project" or item.running then return launch(item,saved.templates[item.value]) end
    local layouts={{id="shell",title="Start a shell"}}
    flow.layouts={shell=""}; flow.selected=item
    local previous=saved.templates[item.value]
    for i,name in ipairs(flow.projects or {}) do
        local id="layout-"..i
        flow.layouts[id]=name
        local choice={id=id,title=name..(name==previous and " (last used)" or "")}
        if name==previous then table.insert(layouts,1,choice) else table.insert(layouts,choice) end
        if #layouts>=100 then break end
    end
    if #layouts==1 then return launch(item,"") end
    return request("layout",afterglow.select(flow.pane,"Layout for "..providers.label(item.value),layouts))
end
function script.run_action(action,ev)
    if not ev.pane then return nil end
    if flow then return notice("A project operation is already open.",ev.pane.id) end
    flow={action=action,pane=ev.pane.id,cwd=ev.pane.cwd,terminal=ev.terminal,entries={},count=0,pending=0,failed={}}
    for _,window in ipairs(ev.terminal.windows) do
        if window.id==ev.pane.window then
            flow.current=window.project_directory~="" and window.project_directory or ev.pane.cwd
            flow.currentProject=window.project
        end
    end
    return request("state",afterglow.state_read())
end
function script.on_result(ev)
    if not flow then return nil end
    local id=ev.request_id
    if id=="state" then
        if not ev.ok then return finish(ev.error.message) end
        saved,revision=history.normalize(ev.data.value),ev.data.revision
        if flow.action=="favorite" then
            local path=providers.path(flow.current or flow.cwd,home)
            if not path then return finish("The current pane has no local directory.") end
            if not saved.favorites[path] then
                local count=0
                for _ in pairs(saved.favorites) do count+=1 end
                if count>=100 then return finish("Favorite limit reached (100). Unpin another directory first.") end
            end
            saved.favorites[path]=not saved.favorites[path] or nil
            saved=history.normalize(saved)
            flow.message=saved.favorites[path] and "Favorite saved" or "Favorite removed"
            return request("saved",afterglow.state_write(saved,revision))
        elseif flow.action=="previous" then
            for _,item in ipairs(saved.recent) do
                if (item.kind=="directory" and item.value~=flow.current)
                    or (item.kind=="project" and item.value~=flow.currentProject) then
                    return launch(item,saved.templates[item.value])
                end
            end
            return finish("No previous project yet.")
        end
        for path in pairs(saved.favorites) do add("directory",path) end
        for _,item in ipairs(saved.recent) do add(item.kind,item.value) end
        for _,window in ipairs(ev.terminal.windows) do
            if window.project_directory and window.project_directory~="" then add("directory",window.project_directory,true) end
            if window.project and window.project~="" then add("project",window.project,true) end
        end
        local jobs,roots=providers.jobs(config,home)
        for _,root in ipairs(roots) do add("directory",root) end
        local effects={request("projects",afterglow.list_projects())}
        flow.pending=1+#jobs; flow.jobs={}
        for _,job in ipairs(jobs) do
            flow.jobs[job.id]=job.kind
            table.insert(effects,request(job.id,afterglow.exec(job.id,job.command,{timeout_ms=5000,max_bytes=131072})))
        end
        return effects
    elseif id=="projects" or (flow.jobs and flow.jobs[id]) then
        if id=="projects" then
            flow.projects=ev.ok and ev.data.names or {}
            for _,name in ipairs(flow.projects) do add("project",name) end
        elseif ev.ok and ev.data.exit_code==0 then
            for _,path in ipairs(providers.parse(flow.jobs[id],ev.data.stdout or "",home)) do add("directory",path) end
        else table.insert(flow.failed,id) end
        flow.pending-=1
        return picker()
    elseif id=="choose" then
        if not ev.ok then return finish() end
        local item=flow.choices[ev.data.id]
        if not item then return finish("Selection expired") end
        return choose(item)
    elseif id=="layout" then
        if not ev.ok then return finish() end
        local layout=flow.layouts[ev.data.id]
        if layout==nil then return finish("Selection expired") end
        return launch(flow.selected,layout)
    elseif id=="opened" then
        if not ev.ok then return finish(ev.error.message) end
        if ev.data.cwd then flow.selected.value=ev.data.cwd end
        saved=history.remember(saved,flow.selected,flow.layout)
        return request("saved",afterglow.state_write(saved,revision))
    elseif id=="saved" then
        if not ev.ok then return finish("History was not saved: "..ev.error.message) end
        revision=ev.data.revision
        return finish(flow.message)
    end
end
return script
