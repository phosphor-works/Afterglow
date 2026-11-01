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
function segments.git(output)
    local branch=string.match(output,"^## ([^\n]+)")
    if not branch then return "" end
    branch=string.gsub(branch,"^No commits yet on ","")
    branch=string.gsub(branch,"^Initial commit on ","")
    branch=string.match(branch,"^(.-)%.%.%.") or branch
    local dirty=string.find(output,"\n[^\n]")~=nil
    return clean(branch,80)..(dirty and " *" or "")
end
function segments.render(pane,git,exit,config)
    local cwd=clean(string.match(pane.cwd or "","([^/]+)/*$") or pane.cwd,80)
    local command=clean(pane.command,120)
    local values={cwd=cwd,git=git or "",command=command~="" and command or "idle",
        exit=command=="" and exit and (exit==0 and "ok" or "exit "..exit) or "",context=clean(pane.context,80)}
    local parts={}
    for i,key in ipairs(type(config.segments)=="table" and config.segments or {"cwd","git","command","exit","context"}) do
        if i>8 then break end
        if values[key] and values[key]~="" then table.insert(parts,values[key]) end
    end
    local separator=type(config.separator)=="string" and clean(config.separator,16) or "  |  "
    local status=trim(table.concat(parts,separator),500)
    local title=config.tab_title~=false and trim(cwd..(command~="" and " • "..command or ""),120) or ""
    return status,title
end
return segments
