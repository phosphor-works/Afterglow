-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Select and copy a retained shell prompt, input or output region.
local flow
local function request(id, effect) return afterglow.request(id, effect) end
return {
    metadata = {name="Shell regions", actions={{id="copy", title="Copy a shell semantic region"}}},
    run_action = function(_, ev)
        if not ev.pane or flow then return nil end
        flow = {pane=ev.pane.id}
        return request("regions", afterglow.semantic_regions(flow.pane))
    end,
    on_result = function(ev)
        if not flow then return nil end
        if not ev.ok then
            local pane=flow.pane; flow=nil
            if ev.error.code~="cancelled" then return afterglow.notify("Shell regions",ev.error.message,pane) end
            return nil
        end
        if ev.request_id=="regions" then
            local choices={}
            for i=#ev.data.regions,1,-1 do
                local region=ev.data.regions[i]
                if region.available then
                    table.insert(choices,{id=region.id,title=region.kind.." #"..region.id
                        ..(region.open and " (open)" or "")..(region.start_trimmed and " (trimmed)" or "")})
                    if #choices==200 then break end
                end
            end
            if #choices==0 then
                local pane=flow.pane; flow=nil
                return afterglow.notify("Shell regions","No retained shell markers in this pane.",pane)
            end
            return request("choice",afterglow.select(flow.pane,"Copy shell region",choices))
        elseif ev.request_id=="choice" then
            return request("text",afterglow.read_region(flow.pane,ev.data.id,262144))
        elseif ev.request_id=="text" then
            local pane=flow.pane; flow=nil
            if not ev.data.available then return afterglow.notify("Shell regions","The region expired.",pane) end
            if ev.data.truncated then return {afterglow.copy(ev.data.text),afterglow.notify("Shell regions","Copied the first 256 KiB.",pane)} end
            return afterglow.copy(ev.data.text)
        end
    end,
    on_pane_closed = function(ev) if flow and ev.pane.id==flow.pane then flow=nil end end,
}
