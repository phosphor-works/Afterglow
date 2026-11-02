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
    component={fg=15,bg=8}, accent={fg=0,bg=6},
}
local function color(config,name)
    local custom=type(config.colors)=="table" and config.colors[name]
    return type(custom)=="table" and custom or Defaults[name]
end
-- One window's tab line, like tabline.wez: the project on the left, then the
-- tabs, then the focused pane's components against the right edge. tabs are
-- in their window order; values are segments.values of the focused pane.
function segments.line(window,tabs,values,config)
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
    local project=clean(window.project,40)
    if project~="" then block(project,color(config,"section"),nil,true) end
    local max=math.clamp(tonumber(config.tab_max_length) or 24,4,80)
    for _,tab in ipairs(tabs) do
        local title=trim(clean(tab.title,200),max)
        block(tostring(tab.index+1).." "..(title~="" and title or "shell"),
            color(config,tab.active and "active" or "inactive"),"tab:"..tab.id)
    end
    if previous and powerline then add({text=Right,fg=previous.bg}) end
    add({spacer=true})
    -- The pane components, from the configured list, against the right edge.
    local names=type(config.segments)=="table" and config.segments or {"command","exit","git","cwd","context"}
    local shown={}
    for i,name in ipairs(names) do
        if i>8 then break end
        if values[name] and values[name]~="" then table.insert(shown,values[name]) end
    end
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
