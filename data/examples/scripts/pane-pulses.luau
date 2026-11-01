-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copy into ~/.config/afterglow/scripts. Needs Shaders.paneEffects.
-- Pulses panes: a red flash when a command fails, a green portal when a
-- command of ten seconds or more succeeds, and a shake on the bell.
return {
    metadata = { name = "Pane pulses" },
    on_command_finished = function(ev)
        if ev.exit_code > 0 then
            return afterglow.pulse(ev.pane, "flash", { color = 1, duration_ms = 700 })
        end
        if ev.exit_code == 0 and ev.duration_ms >= 10000 then
            return afterglow.pulse(ev.pane, "portal", { color = 2, duration_ms = 900 })
        end
        return nil
    end,
    on_bell = function(ev)
        return afterglow.pulse(ev.pane, "shake", { duration_ms = 450 })
    end,
}
