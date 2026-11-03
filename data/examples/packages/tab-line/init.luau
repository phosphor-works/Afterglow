-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local segments=require("segments")
local script={metadata={name="Tab line",actions={{id="toggle",title="Toggle the tab line"}}}}
local config,enabled,tick={},true,0
local git,exits,job,jobs={}, {}, nil, 0
local toggling,togglePane=nil,0
-- Event redraws closer together than this go out as one.
local DrawMs=250
-- By window: shown is the line that Afterglow accepted, pending the line that
-- waits for its result, refused a line that Afterglow will not take.
local shown,pending,refused,sends={}, {}, {}, 0
-- By window: the line's width in cells once it shows, the item under the
-- pointer, and the item under a drag.
local widths,hovered,dragged={}, {}, {}
-- The tick of the draw timer's start or last call, while it runs. dirty marks
-- a change that the timer must still draw.
local drawing,dirty=nil,false
-- A notice needs a pane that is still open. Without one, the message goes to
-- the log that `afterglow ctl scripts` and the afterglow.scripts category show.
local function failure(message,terminal)
    for _,pane in ipairs(terminal.panes) do
        if pane.id==togglePane then return afterglow.notify("Tab line",message,pane.id) end
    end
    return afterglow.log(message)
end
-- One bounded Git query: no shell, no network, no optional locks.
-- core.fsmonitor in the repository's own config would run a command.
local function query(args)
    local argv={"git","-c","core.fsmonitor=false","--no-optional-locks","-C",job.cwd}
    for _,arg in ipairs(args) do table.insert(argv,arg) end
    job.tick=tick
    return afterglow.request("git:"..job.id,afterglow.exec("git",argv,{timeout_ms=2000,max_bytes=16384}))
end
-- The window's focused pane, or any of its panes while focus moves.
local function windowPane(terminal,window)
    local any
    for _,pane in ipairs(terminal.panes) do
        if pane.id==window.active_pane then return pane end
        if pane.window==window.id then any=any or pane end
    end
    return any
end
-- The window's tabs in their order.
local function windowTabs(terminal,window)
    local tabs={}
    for _,tab in ipairs(terminal.tabs) do
        if tab.window==window then table.insert(tabs,tab) end
    end
    table.sort(tabs,function(a,b) return a.index<b.index end)
    return tabs
end
-- A line as one string, so that an unchanged line is not sent again.
local function key(runs)
    local parts={}
    for _,run in ipairs(runs) do
        table.insert(parts,table.concat({run.text or "~",tostring(run.fg),tostring(run.bg),tostring(run.item)},"\1"))
    end
    return table.concat(parts,"\2")
end
-- The line that the window must show, as runs and their key. "" is no line.
local function wanted(terminal,window,pane)
    local tabs=windowTabs(terminal,window.id)
    if not enabled or (#tabs<2 and config.single_tab==false) then return nil,"" end
    local entry=(pane.context or "")=="" and git[pane.cwd] or nil
    local values=segments.values(pane,entry and entry.label,exits[pane.id])
    -- Until the line shows, it is as wide as the window.
    local width=widths[window.id] or window.cols
    local runs=segments.line(window,tabs,values,config,width,hovered[window.id])
    return runs,key(runs)
end
-- Sends each window's tab line, drawn in place of its tab bar, when it differs
-- from the line that Afterglow accepted. Each window has one line on its way
-- at a time. Its result moves it to shown (see on_result), so a line that
-- Afterglow drops goes out again at the next draw. Returns the effects, and
-- whether a window still waits for its line.
local function flush(terminal)
    local effects,live,behind={}, {}, false
    for _,window in ipairs(terminal.windows) do
        live[window.id]=true
        local pane=windowPane(terminal,window)
        local runs,line
        if pane then runs,line=wanted(terminal,window,pane) end
        if pane and line~=(shown[window.id] or "") and line~=refused[window.id] then
            if pending[window.id] then
                behind=true
            else
                sends+=1
                pending[window.id]={id=sends,line=line,tick=tick}
                local effect=runs and afterglow.dock(pane,{id="tabline",edge="top",tab_bar=true,lines={runs}})
                    or afterglow.undock(pane,"tabline")
                table.insert(effects,afterglow.request("line:"..window.id..":"..sends,effect))
            end
        end
    end
    -- Closed windows take their areas with them.
    for _,byWindow in ipairs({shown,pending,refused,widths,hovered,dragged}) do
        for id in pairs(byWindow) do
            if not live[id] then byWindow[id]=nil end
        end
    end
    return effects,behind
end
-- Tabs, titles, focus and commands change what the line shows. The first
-- change draws at once. The draw timer then collects the changes that follow,
-- so a title that changes many times a second costs few lines.
local function redraw(ev)
    if drawing then dirty=true;return nil end
    local effects,behind=flush(ev.terminal)
    drawing,dirty=tick,behind
    -- A timer that the age check below gave up on can still run.
    table.insert(effects,afterglow.cancel("draw"))
    table.insert(effects,afterglow.timer("draw",DrawMs,DrawMs))
    return effects
end
-- A change that the draw timer draws, when it next calls.
local function drawLater()
    dirty=true
    if drawing then return nil end
    drawing=tick
    return {afterglow.cancel("draw"),afterglow.timer("draw",DrawMs,DrawMs)}
end
-- Lines that drawNow sends in each refresh second. Past them the draw timer
-- draws, so that a fast sweep of the pointer over many tabs stays inside
-- the dock budget and the result limit. The budget is any 1000 ms, so the
-- end of one refresh second and the start of the next can both fall in it:
-- twice this, the draw timer's 4 and the refresh are under 60.
local NowLines=25
local nowLines,nowTick=0,0
-- The pointer and the line's width draw at once, so that a hover shows
-- while the pointer is still there. A window whose line is on its way gets
-- the draw timer.
local function drawNow(ev)
    if nowTick~=tick then nowTick,nowLines=tick,0 end
    if nowLines>=NowLines then return drawLater() end
    local before=sends
    local effects,behind=flush(ev.terminal)
    nowLines+=sends-before
    if behind and drawing then
        dirty=true
    elseif behind then
        drawing,dirty=tick,true
        table.insert(effects,afterglow.cancel("draw"))
        table.insert(effects,afterglow.timer("draw",DrawMs,DrawMs))
    end
    return effects
end
function script.on_load(ev)
    config=ev.package.config;enabled=ev.storage.value.enabled~=false
    -- Draw now: the previous instance's line went when it unloaded.
    local effects=flush(ev.terminal)
    table.insert(effects,afterglow.timer("refresh",1000,1000))
    return effects
end
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
    return redraw(ev)
end
function script.on_pane_closed(ev)
    exits[ev.pane.id]=nil
    return redraw(ev)
end
local function tabById(terminal,id)
    for _,tab in ipairs(terminal.tabs) do
        if tab.id==id and tab.active_pane~=0 then return tab end
    end
    return nil
end
-- As on the tab bar: a left click shows a tab, a middle click closes it and
-- a right click opens its menu. "+N" shows the nearest hidden tab, and "+"
-- opens a new tab.
function script.on_dock_click(ev)
    if ev.dock~="tabline" then return nil end
    if ev.item=="new" then
        return ev.button=="left" and ev.pane and afterglow.new_tab(ev.pane) or nil
    end
    local kind,id=string.match(ev.item,"^(%a+):(%d+)$")
    local tab=tabById(ev.terminal,tonumber(id))
    if not tab then return nil end
    if ev.button=="left" then return afterglow.focus(tab.active_pane) end
    if kind~="tab" then return nil end
    if ev.button=="middle" then return afterglow.action(tab.active_pane,"close_tab") end
    return afterglow.tab_menu(tab.active_pane)
end
-- The wheel over the line moves through the tabs, and stops at the ends.
function script.on_dock_wheel(ev)
    if ev.dock~="tabline" or (ev.direction~="up" and ev.direction~="down") then return nil end
    local tabs=windowTabs(ev.terminal,ev.window)
    for i,tab in ipairs(tabs) do
        if tab.active then
            local target=tabs[math.clamp(i+(ev.direction=="up" and -ev.steps or ev.steps),1,#tabs)]
            if target~=tab and target.active_pane~=0 then return afterglow.focus(target.active_pane) end
            return nil
        end
    end
    return nil
end
-- A tab that is dragged onto another tab, or onto "+N", takes its place.
-- Only a new item under the pointer moves it: the line that shows the move
-- comes after the move (on_pane_moved), and until then the pointer is over
-- the old cells. Moves count toward the callback limit, so the end moves
-- the tab too when the last item that it saw is not the one under it.
function script.on_dock_drag(ev)
    if ev.dock~="tabline" then return nil end
    local id=tonumber(string.match(ev.item,"^tab:(%d+)$"))
    if not id or ev.phase=="cancel" then dragged[ev.window]=nil;return nil end
    if ev.phase=="start" then dragged[ev.window]=nil end
    local last=dragged[ev.window]
    dragged[ev.window]=if ev.phase=="end" then nil else ev.over
    if ev.over==last then return nil end
    local target=ev.over and tonumber(string.match(ev.over,"^%a+:(%d+)$"))
    local from,to=tabById(ev.terminal,id),target and tabById(ev.terminal,target)
    if not from or not to or from==to or from.window~=to.window then return nil end
    return afterglow.move_tab(from.active_pane,to.index)
end
-- A pointer that leaves an item draws later: between two tabs, the cells of
-- the separator are on no item, and the next tab comes before the timer.
function script.on_dock_hover(ev)
    if ev.dock~="tabline" then return nil end
    hovered[ev.window]=ev.item
    if ev.item==nil then return drawLater() end
    return drawNow(ev)
end
-- The line lays its tabs out for the cells that it has.
function script.on_dock_layout(ev)
    if ev.dock~="tabline" then return nil end
    widths[ev.window]=ev.shown and ev.cols or nil
    return drawNow(ev)
end
local function refresh(ev)
    tick+=1
    local effects={}
    -- State that waits for a call that never came: a failed handler call,
    -- or a timer that Afterglow refused at the effect limit. Results come
    -- within milliseconds, a Git query within its 2 s timeout, and the draw
    -- timer calls 4 times a second.
    if drawing and tick-drawing>=2 then drawing=nil end
    for id,line in pairs(pending) do
        if tick-line.tick>=2 then pending[id]=nil end
    end
    if job and tick-job.tick>=5 then
        job=nil
        table.insert(effects,afterglow.cancel("git"))
    end
    if toggling and tick-toggling>=5 then toggling=nil end
    for _,effect in ipairs((flush(ev.terminal))) do table.insert(effects,effect) end
    if not enabled or config.git==false or job then return effects end
    -- One query at a time, for the focused pane of each window in turn. The
    -- branch comes from HEAD alone: a command that compares the work tree
    -- runs the clean filters that the repository's own config names, and
    -- terminal output can set pane.cwd to any directory.
    local seconds=math.clamp(tonumber(config.git_refresh_seconds) or 15,5,300)
    for _,window in ipairs(ev.terminal.windows) do
        local pane=windowPane(ev.terminal,window)
        local cached=pane and git[pane.cwd]
        if pane and pane.cwd~="" and (pane.context or "")=="" and (not cached or tick-cached.tick>=seconds) then
            jobs+=1
            job={id=jobs,cwd=pane.cwd,stage="branch"}
            table.insert(effects,query({"symbolic-ref","--short","-q","HEAD"}))
            break
        end
    end
    -- Bound the cache: pane.cwd can name any number of directories.
    local count=0;for _ in pairs(git) do count+=1 end
    if count>128 then git={} end
    return effects
end
function script.on_timer(ev)
    if ev.name=="refresh" then return refresh(ev) end
    if ev.name~="draw" then return nil end
    -- A timer that the age check gave up on stops here.
    if not drawing then return afterglow.cancel("draw") end
    if not dirty then
        drawing=nil
        return afterglow.cancel("draw")
    end
    local effects,behind=flush(ev.terminal)
    drawing,dirty=tick,behind
    return effects
end
function script.run_action(action,ev)
    if action~="toggle" or toggling then return nil end
    toggling=tick;togglePane=ev.pane and ev.pane.id or ev.terminal.active_pane
    return afterglow.request("state",afterglow.state_read())
end
local function gitResult(ev)
    local data,code=ev.data or {},ev.error and ev.error.code
    if code=="rate_limited" or code=="busy" then
        -- The query did not run, so it says nothing about the directory.
        job=nil
        return nil
    end
    local output=ev.ok and data.stdout or ""
    if job.stage=="branch" and not ev.ok and data.exit_code==1 then
        -- A detached HEAD is not a symbolic ref: show the commit instead.
        job.stage="commit"
        return query({"rev-parse","--short","-q","--verify","HEAD"})
    end
    if job.stage~="changes" and output~="" and config.git_changes==true then
        job.stage="changes";job.branch=output
        return query({"status","--porcelain=v1","--untracked-files=no"})
    end
    local label
    if job.stage=="changes" then
        -- Status output over the limit still lists changes.
        local status=(ev.ok or code=="output_limit") and data.stdout or nil
        label=segments.git(job.branch,status)
    else
        label=segments.git(output)
    end
    git[job.cwd]={label=label,tick=tick};job=nil
    return (flush(ev.terminal))
end
function script.on_result(ev)
    local window,id=string.match(ev.request_id,"^line:(%d+):(%d+)$")
    if window then
        window=tonumber(window)
        local line=pending[window]
        if not line or line.id~=tonumber(id) then return nil end
        pending[window]=nil
        if ev.ok then
            shown[window]=line.line~="" and line.line or nil
            refused[window]=nil
        elseif ev.error.code=="invalid_effect" then
            -- The same line would be refused again. A changed line is sent.
            refused[window]=line.line
        end
        -- A line that was dropped, for example over the effect limit, goes
        -- out at the next draw, not from here: a refusal that came back at
        -- once would send results faster than Afterglow takes them.
        return nil
    end
    local jobId=tonumber(string.match(ev.request_id,"^git:(%d+)$"))
    if jobId then
        if not job or job.id~=jobId then return nil end
        return gitResult(ev)
    elseif ev.request_id=="state" then
        if not ev.ok then toggling=nil;return failure(ev.error.message,ev.terminal) end
        return afterglow.request("saved",afterglow.state_write({enabled=ev.data.value.enabled==false},ev.data.revision))
    elseif ev.request_id=="saved" then
        toggling=nil
        if not ev.ok then return failure(ev.error.message,ev.terminal) end
        enabled=ev.data.value.enabled~=false
        -- Off, the lines go and the windows get their tab bars back.
        return (flush(ev.terminal))
    end
    return nil
end
return script
