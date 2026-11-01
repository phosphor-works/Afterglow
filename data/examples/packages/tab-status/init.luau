-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local segments=require("segments")
local script={metadata={name="Tab and repository status",actions={{id="toggle",title="Toggle tab and repository status"}}}}
local config,enabled,tick,cursor={},true,0,0
local git,exits,shown,job={}, {}, {}, nil
local toggling,togglePane=false,0
-- A notice needs a pane. Without one, the message goes to the script log.
local function failure(message)
    if togglePane==0 then return afterglow.log(message) end
    return afterglow.notify("Tab status",message,togglePane)
end
function script.on_load(ev)
    config=ev.package.config;enabled=ev.storage.value.enabled~=false
    return afterglow.timer("refresh",1000,1000)
end
local function render(pane)
    local entry=(pane.context or "")=="" and git[pane.cwd] or nil
    local text,title=segments.render(pane,entry and entry.label or "",exits[pane.id],config)
    local value=text.."\0"..title
    if shown[pane.id]==value then return nil end
    shown[pane.id]=value
    return afterglow.status(pane,text,title)
end
function script.on_command_finished(ev)
    exits[ev.pane.id]=ev.exit_code>=0 and ev.exit_code or nil
    if git[ev.pane.cwd] then git[ev.pane.cwd].tick=-1000 end
end
function script.on_pane_closed(ev) exits[ev.pane.id]=nil;shown[ev.pane.id]=nil end
function script.on_timer(ev)
    if ev.name~="refresh" then return nil end
    tick+=1
    local candidates={}
    for _,tab in ipairs(ev.terminal.tabs) do
        for _,pane in ipairs(ev.terminal.panes) do
            if pane.id==tab.active_pane then table.insert(candidates,pane);break end
        end
    end
    -- Clear contributions on panes that no longer own their tab's title.
    local active={};for _,p in ipairs(candidates) do active[p.id]=true end
    local effects={}
    local cleared=0
    for _,p in ipairs(ev.terminal.panes) do
        if shown[p.id] and (not enabled or not active[p.id]) and cleared<8 then
            shown[p.id]=nil;cleared+=1;table.insert(effects,afterglow.status(p,"",""))
        end
    end
    if not enabled or #candidates==0 then return effects end
    -- The focused pane updates every tick. One other tab rotates in each tick.
    local panes={}
    for _,p in ipairs(candidates) do
        if p.id==ev.terminal.active_pane then table.insert(panes,p) end
    end
    cursor=cursor % #candidates+1
    if not panes[1] or panes[1].id~=candidates[cursor].id then table.insert(panes,candidates[cursor]) end
    for _,pane in ipairs(panes) do
        local update=render(pane);if update then table.insert(effects,update) end
    end
    local refresh=math.clamp(tonumber(config.git_refresh_seconds) or 15,5,300)
    for _,pane in ipairs(panes) do
        local cached=git[pane.cwd]
        if config.git~=false and not job and pane.cwd~="" and (pane.context or "")==""
            and (not cached or tick-cached.tick>=refresh) then
            -- One bounded Git query at a time; no shell, no network, no optional locks.
            -- core.fsmonitor in the repository's own config would run a command.
            job={cwd=pane.cwd}
            table.insert(effects,afterglow.request("git",afterglow.exec("git",{"git","-c","core.fsmonitor=false",
                "--no-optional-locks","-C",pane.cwd,"status","--porcelain=v1","--branch","--untracked-files=no"},
                {timeout_ms=2000,max_bytes=16384})))
        end
    end
    -- Bound the cache even across long-running navigation sessions.
    local count=0;for _ in pairs(git) do count+=1 end
    if count>128 then git={} end
    return effects
end
function script.run_action(action,ev)
    if action~="toggle" or toggling then return nil end
    toggling=true;togglePane=ev.pane and ev.pane.id or ev.terminal.active_pane
    return afterglow.request("state",afterglow.state_read())
end
function script.on_result(ev)
    if ev.request_id=="git" then
        if job then git[job.cwd]={label=ev.ok and segments.git(ev.data.stdout or "") or "",tick=tick};job=nil end
    elseif ev.request_id=="state" then
        if not ev.ok then toggling=false;return failure(ev.error.message) end
        return afterglow.request("saved",afterglow.state_write({enabled=ev.data.value.enabled==false},ev.data.revision))
    elseif ev.request_id=="saved" then
        toggling=false
        if not ev.ok then return failure(ev.error.message) end
        enabled=ev.data.value.enabled~=false
    end
end
return script
