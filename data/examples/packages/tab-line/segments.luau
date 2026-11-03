-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local segments={}
-- Cuts text to at most max bytes without splitting a UTF-8 sequence.
local function trim(text,max)
    if #text<=max then return text end
    return string.sub(text,1,utf8.offset(text,0,max+1)-1)
end
local function clean(text,max)
    return trim((string.gsub(type(text)=="string" and text or "","%c"," ")),max)
end
-- The branch (or commit) from Git's output, and " *" when the optional
-- porcelain status lists a tracked change.
function segments.git(branch,status)
    branch=string.match(branch or "","^%s*(.-)%s*$")
    if branch=="" then return "" end
    local dirty=status~=nil and string.find(status,"%S")~=nil
    return clean(branch,80)..(dirty and " *" or "")
end
-- The text of each pane component.
function segments.values(pane,git,exit)
    local command=clean(pane.command,80)
    return {
        cwd=clean(string.match(pane.cwd or "","([^/]+)/*$") or pane.cwd,60),
        git=git or "",
        command=command,
        exit=command=="" and exit and (exit==0 and "ok" or "exit "..exit) or "",
        context=clean(pane.context,60),
    }
end
-- Powerline separators: the renderer draws them, so no special font is needed.
local Right,Left="\u{e0b0}","\u{e0b2}"
local Defaults={
    section={fg=0,bg=4}, active={fg=0,bg=4}, inactive={fg=7,bg=8},
    component={fg=15,bg=8}, accent={fg=0,bg=6}, hover={fg=0,bg=7},
}
-- A block's colors. A configured block that gives only fg or bg keeps the
-- default of the other.
local function color(config,name)
    local custom=type(config.colors)=="table" and config.colors[name]
    local default=Defaults[name]
    if type(custom)~="table" then return default end
    return {fg=custom.fg or default.fg,bg=custom.bg or default.bg}
end
-- Afterglow takes at most 128 runs in a line. Each tab takes 2, and the other
-- parts at most 25.
local MaxTabs=48
-- The cells of a block: its text, a space on each side and the separator
-- before it. UTF-8 bytes are never fewer than the cells that the text takes.
local function cells(text) return #text+3 end
-- One window's tab line, like tabline.wez: the project on the left, then the
-- tabs, then the focused pane's components against the right edge. tabs are
-- in their window order; values are segments.values of the focused pane.
-- width is the line's width in cells when it is known. Tabs that do not
-- fit it, or past MaxTabs, show as "+N" on each side of the current tab.
-- hovered is the item under the pointer: a tab or button that is not
-- current shows in the hover colors.
function segments.line(window,tabs,values,config,width,hovered)
    local powerline=config.powerline~=false
    local runs={}
    local function add(run) table.insert(runs,run) end
    -- A block of text in colors c, then the separator into the next block's background.
    local previous
    local function block(text,c,item,bold)
        if previous then
            if powerline then add({text=Right,fg=previous.bg,bg=c.bg}) else add({text=" ",bg=c.bg}) end
        end
        add({text=" "..text.." ",fg=c.fg,bg=c.bg,bold=bold,item=item})
        previous=c
    end
    local function tint(name,item)
        return color(config,item and item==hovered and name~="active" and "hover" or name)
    end
    local newTab=config.new_tab_button~=false
    local project=clean(window.project,40)
    local max=math.clamp(tonumber(config.tab_max_length) or 24,4,80)
    local titles={}
    for i,tab in ipairs(tabs) do
        local title=trim(clean(tab.title,200),max)
        titles[i]=tostring(tab.index+1).." "..(title~="" and title or "shell")
    end
    -- The pane components, from the configured list, against the right edge.
    local names=type(config.segments)=="table" and config.segments or {"command","exit","git","cwd","context"}
    local shown={}
    for i,name in ipairs(names) do
        if i>8 then break end
        if values[name] and values[name]~="" then table.insert(shown,values[name]) end
    end
    -- The tabs that fit between the project and the components: all of them,
    -- or the current tab and its neighbors.
    local first,last=1,#tabs
    -- cells counts a separator before every block and component. The first
    -- block has none, and with powerline the left side ends in one. Without
    -- powerline, the first component has none either.
    local edges=powerline and 0 or (#shown>0 and 2 or 1)
    local room=(width or math.huge)+edges-(project~="" and cells(project) or 0)-(newTab and cells("+") or 0)
    local need=0
    for _,text in ipairs(shown) do room-=cells(text) end
    for _,text in ipairs(titles) do need+=cells(text) end
    if #tabs>0 and (#tabs>MaxTabs or need>room) then
        for i,tab in ipairs(tabs) do
            if tab.active then first=i end
        end
        last=first
        room-=2*cells("+"..#tabs)
        local used,grew=cells(titles[first]),true
        while grew do
            grew=false
            if last-first+1<MaxTabs and titles[last+1] and used+cells(titles[last+1])<=room then
                last+=1;used+=cells(titles[last]);grew=true
            end
            if last-first+1<MaxTabs and titles[first-1] and used+cells(titles[first-1])<=room then
                first-=1;used+=cells(titles[first]);grew=true
            end
        end
    end
    if project~="" then block(project,color(config,"section"),nil,true) end
    -- A click on "+N" goes to the nearest hidden tab.
    if first>1 then
        local item="more:"..tabs[first-1].id
        block("+"..(first-1),tint("inactive",item),item)
    end
    for i=first,last do
        local item="tab:"..tabs[i].id
        block(titles[i],tint(tabs[i].active and "active" or "inactive",item),item)
    end
    if last<#tabs then
        local item="more:"..tabs[last+1].id
        block("+"..(#tabs-last),tint("inactive",item),item)
    end
    if newTab then block("+",tint("inactive","new"),"new") end
    if previous and powerline then add({text=Right,fg=previous.bg}) end
    add({spacer=true})
    local before
    for i,text in ipairs(shown) do
        local c=color(config,i==#shown and "accent" or "component")
        if powerline then add({text=Left,fg=c.bg,bg=before and before.bg}) elseif i>1 then add({text=" "}) end
        add({text=" "..text.." ",fg=c.fg,bg=c.bg})
        before=c
    end
    return runs
end
return segments
