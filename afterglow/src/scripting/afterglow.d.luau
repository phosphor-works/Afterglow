-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Type definitions for afterglow scripts, for editors with Luau support.
--
-- A script is a .luau file in ~/.config/afterglow/scripts/ (or the system
-- scripts directory) that returns a module table:
--
--   local script = {}
--   script.metadata = {
--       id = "my-script",              -- optional, defaults to the file name
--       name = "My Script",
--       description = "What it does",
--       actions = { { id = "hello", title = "Say Hello" } },
--   }
--   function script.on_command_finished(ev: CommandFinishedEvent): Effects? ... end
--   function script.run_action(action: string, ev: ActionEvent): Effects? ... end
--   return script
--
-- Handlers run sandboxed with a short time limit and no access to files or
-- processes. They return an effect, a list of effects, or nil. Actions appear
-- in the command palette and bind in Keybinds.bindings as
-- "script:<id>.<action>".

export type Pane = {
    id: number,
    window: number,
    tab: number,
    title: string,
    cwd: string,
    command: string, -- running now (OSC 133), "" at the prompt
    cols: number,
    rows: number,
    focused: boolean,
}

export type PaneEvent = { pane: Pane }
export type TitleEvent = { pane: Pane, title: string }
export type CommandStartedEvent = { pane: Pane, command: string }
export type CommandFinishedEvent = {
    pane: Pane,
    command: string,
    exit_code: number, -- -1 when the shell did not report one
    duration_ms: number,
}
export type StatusEvent = { pane: Pane, state: string, text: string }
export type ActionEvent = { pane: Pane? } -- the focused pane, if any

export type Effect = { kind: string }
export type Effects = Effect | { Effect }

-- Handlers a script may define, all optional:
--   on_pane_created(ev: PaneEvent)        on_pane_closed(ev: { pane: { id: number } })
--   on_focus(ev: PaneEvent)               on_bell(ev: PaneEvent)
--   on_title(ev: TitleEvent)              on_status(ev: StatusEvent)
--   on_command_started(ev: CommandStartedEvent)
--   on_command_finished(ev: CommandFinishedEvent)
--   run_action(action: string, ev: ActionEvent)

type PaneRef = Pane | number

declare afterglow: {
    send_text: (pane: PaneRef, text: string) -> Effect,
    -- pane defaults to the event's pane
    notify: (title: string, body: string?, pane: PaneRef?) -> Effect,
    split: (pane: PaneRef, direction: ("right" | "down")?, command: (string | { string })?) -> Effect,
    new_tab: (pane: PaneRef, cwd: string?, command: (string | { string })?) -> Effect,
    new_window: (cwd: string?, command: (string | { string })?) -> Effect,
    focus: (pane: PaneRef) -> Effect,
    close: (pane: PaneRef) -> Effect,
    set_title: (pane: PaneRef, title: string) -> Effect, -- "" returns to the program's title
    copy: (text: string) -> Effect,
    open_url: (url: string) -> Effect, -- http, https and mailto
    action: (pane: PaneRef, name: string) -> Effect, -- a keybind action: split_right, new_tab...
    -- sets iSignal[channel] (0-7) on the pane's shaders to (value, iTime now)
    signal: (pane: PaneRef, channel: number, value: number?) -> Effect,
    log: (message: string) -> Effect,
}
