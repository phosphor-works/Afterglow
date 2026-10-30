-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
local segments={}
local function clean(text,max)
    return string.sub(string.gsub(type(text)=="string" and text or "","%c"," "),1,max)
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
    local status=string.sub(table.concat(parts,separator),1,500)
    local title=config.tab_title~=false and string.sub(cwd..(command~="" and " • "..command or ""),1,120) or ""
    return status,title
end
return segments
