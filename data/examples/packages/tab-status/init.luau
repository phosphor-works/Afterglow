-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local segments=require("segments")
local script={metadata={name="Tab line",actions={{id="toggle",title="Toggle the tab line"}}}}
local config,enabled,tick={},true,0
local git,exits,shown,job={}, {}, {}, nil
local toggling,togglePane=false,0
-- A notice needs a pane. Without one, the message goes to the script log.
local function failure(message)
    if togglePane==0 then return afterglow.log(message) end
    return afterglow.notify("Tab line",message,togglePane)
end
function script.on_load(ev)
    config=ev.package.config;enabled=ev.storage.value.enabled~=false
    return afterglow.timer("refresh",1000,1000)
end
-- One bounded Git query: no shell, no network, no optional locks.
-- core.fsmonitor in the repository's own config would run a command.
local function query(cwd,args)
    local argv={"git","-c","core.fsmonitor=false","--no-optional-locks","-C",cwd}
    for _,arg in ipairs(args) do table.insert(argv,arg) end
    return afterglow.request("git",afterglow.exec("git",argv,{timeout_ms=2000,max_bytes=16384}))
end
local function focusedPane(terminal,window)
    for _,pane in ipairs(terminal.panes) do
        if pane.id==window.active_pane then return pane end
    end
    return nil
end
-- A line as one string, so that an unchanged line is not sent again.
local function key(runs)
    local parts={}
    for _,run in ipairs(runs) do
        table.insert(parts,table.concat({run.text or "~",tostring(run.fg),tostring(run.bg),tostring(run.item)},"\1"))
    end
    return table.concat(parts,"\2")
end
-- Each window's tab line, drawn in place of its tab bar.
local function render(terminal)
    local effects={}
    if not enabled then return effects end
    for _,window in ipairs(terminal.windows) do
        local pane=focusedPane(terminal,window)
        if pane then
            local tabs={}
            for _,tab in ipairs(terminal.tabs) do
                if tab.window==window.id then table.insert(tabs,tab) end
            end
            table.sort(tabs,function(a,b) return a.index<b.index end)
            local entry=(pane.context or "")=="" and git[pane.cwd] or nil
            local runs=segments.line(window,tabs,segments.values(pane,entry and entry.label,exits[pane.id]),config)
            local line=key(runs)
            if shown[window.id]~=line then
                shown[window.id]=line
                table.insert(effects,afterglow.dock(pane,{id="tabline",edge="top",tab_bar=true,lines={runs}}))
            end
        end
    end
    return effects
end
local function undockAll(terminal)
    local effects={}
    for _,window in ipairs(terminal.windows) do
        if shown[window.id] and window.active_pane~=0 then
            table.insert(effects,afterglow.undock(window.active_pane,"tabline"))
        end
    end
    shown={}
    return effects
end
-- Tabs, titles, focus and commands change what the line shows: draw it at once.
local function redraw(ev) return render(ev.terminal) end
script.on_tab_switch=redraw
script.on_title=redraw
script.on_focus=redraw
script.on_pane_created=redraw
script.on_pane_moved=redraw
script.on_context=redraw
script.on_command_started=redraw
function script.on_command_finished(ev)
    exits[ev.pane.id]=ev.exit_code>=0 and ev.exit_code or nil
    if git[ev.pane.cwd] then git[ev.pane.cwd].tick=-1000 end
    return render(ev.terminal)
end
function script.on_pane_closed(ev)
    exits[ev.pane.id]=nil
    return render(ev.terminal)
end
function script.on_dock_click(ev)
    local id=tonumber(string.match(ev.item,"^tab:(%d+)$"))
    if not id or ev.button~="left" then return nil end
    for _,tab in ipairs(ev.terminal.tabs) do
        if tab.id==id and tab.active_pane~=0 then return afterglow.focus(tab.active_pane) end
    end
    return nil
end
function script.on_timer(ev)
    if ev.name~="refresh" then return nil end
    tick+=1
    local effects=render(ev.terminal)
    if not enabled or config.git==false or job then return effects end
    -- One query at a time, for the focused pane of each window in turn. The
    -- branch comes from HEAD alone: a command that compares the work tree
    -- runs the clean filters that the repository's own config names, and
    -- terminal output can set pane.cwd to any directory.
    local refresh=math.clamp(tonumber(config.git_refresh_seconds) or 15,5,300)
    for _,window in ipairs(ev.terminal.windows) do
        local pane=focusedPane(ev.terminal,window)
        local cached=pane and git[pane.cwd]
        if pane and pane.cwd~="" and (pane.context or "")=="" and (not cached or tick-cached.tick>=refresh) then
            job={cwd=pane.cwd,stage="branch"}
            table.insert(effects,query(pane.cwd,{"symbolic-ref","--short","-q","HEAD"}))
            break
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
        if not job then return nil end
        local output=ev.ok and ev.data.stdout or ""
        if job.stage=="branch" and not ev.ok and ev.data and ev.data.exit_code==1 then
            -- A detached HEAD is not a symbolic ref: show the commit instead.
            job.stage="commit"
            return query(job.cwd,{"rev-parse","--short","-q","--verify","HEAD"})
        end
        if job.stage~="changes" and output~="" and config.git_changes==true then
            job.stage="changes";job.branch=output
            return query(job.cwd,{"status","--porcelain=v1","--untracked-files=no"})
        end
        local label
        if job.stage=="changes" then label=segments.git(job.branch,ev.ok and output or nil) else label=segments.git(output) end
        git[job.cwd]={label=label,tick=tick};job=nil
        return render(ev.terminal)
    elseif ev.request_id=="state" then
        if not ev.ok then toggling=false;return failure(ev.error.message) end
        return afterglow.request("saved",afterglow.state_write({enabled=ev.data.value.enabled==false},ev.data.revision))
    elseif ev.request_id=="saved" then
        toggling=false
        if not ev.ok then return failure(ev.error.message) end
        enabled=ev.data.value.enabled~=false
        -- Off, the windows get their tab bars back.
        if not enabled then return undockAll(ev.terminal) end
        return render(ev.terminal)
    end
end
return script
