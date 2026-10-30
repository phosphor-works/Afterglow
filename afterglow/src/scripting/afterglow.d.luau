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
    tab_id: number, -- stable for the process lifetime; tab is the zero-based position
    title: string,
    cwd: string,
    command: string, -- running now (OSC 133), "" at the prompt
    cols: number,
    rows: number,
    focused: boolean,
    context: string, -- where the shell runs when not here as you ("root", "deploy@prod-1"), else ""
}

export type Window = { id: number, project: string, focused: boolean, active_tab: number, active_pane: number }
export type Tab = { id: number, window: number, index: number, title: string, active: boolean, active_pane: number }
-- Detached metadata, captured before dispatch. Editing it cannot mutate the host.
-- Zero active IDs mean that no terminal window currently has keyboard focus.
export type State = {
    version: number, -- 1
    windows: { Window },
    tabs: { Tab },
    panes: { Pane },
    active_window: number,
    active_tab: number, -- stable ID, not an index
    active_pane: number,
}
export type PaneEvent = { pane: Pane, terminal: State }
export type ClosedPaneEvent = { pane: { id: number }, terminal: State }
export type TitleEvent = { pane: Pane, title: string, terminal: State }
export type CommandStartedEvent = { pane: Pane, command: string, terminal: State }
export type CommandFinishedEvent = {
    pane: Pane,
    command: string,
    exit_code: number, -- -1 when the shell did not report one
    duration_ms: number,
    terminal: State,
}
export type StatusEvent = { pane: Pane, state: string, text: string, terminal: State }
export type ActionEvent = { pane: Pane?, terminal: State } -- the targeted pane, if any
export type UserVarEvent = { pane: Pane, name: string, value: string, terminal: State } -- iTerm2 SetUserVar
export type ContextEvent = { pane: Pane, context: string, terminal: State } -- OSC 3008 / RemoteHost; "" back home

export type OperationError = { code: string, message: string }
export type OperationData = { text: string?, truncated: boolean?, available: boolean?, name: string?, stdout: string?, stderr: string?, exit_code: number?, id: string?, title: string?, pane: Pane?, pane_id: number?, tab_id: number?, window_id: number?, panes: { number }? }
export type ResultEvent = {
    request_id: string,
    kind: string,
    ok: boolean,
    data: OperationData,
    error: OperationError?,
    pane: (Pane | { id: number })?,
    terminal: State, -- fresh at delivery; data describes the earlier operation
}
export type SignalValue = { channel: number, value: number }

export type Effect = { kind: string, request_id: string? }
export type Effects = Effect | { Effect }

-- Handlers a script may define, all optional:
--   on_pane_created(ev: PaneEvent)        on_pane_closed(ev: ClosedPaneEvent)
--   on_pane_moved(ev: PaneEvent) -- same pane ID, new window/tab in the snapshot
--   on_focus(ev: PaneEvent)               on_bell(ev: PaneEvent)
--   on_title(ev: TitleEvent)              on_status(ev: StatusEvent)
--   on_command_started(ev: CommandStartedEvent)
--   on_command_finished(ev: CommandFinishedEvent)
--   on_user_var(ev: UserVarEvent)            on_context(ev: ContextEvent)
--   run_action(action: string, ev: ActionEvent)
--   on_load(ev: ActionEvent)             on_unload(ev: ActionEvent)
--   on_config_changed(ev: ActionEvent)
--   on_timer(ev: ActionEvent & { name: string })
--   on_file_changed(ev: ActionEvent & { name: string, path: string, exists: boolean })
--   on_result(ev: ResultEvent) -- only for effects wrapped in afterglow.request

type PaneRef = Pane | number

declare afterglow: {
    panes: (state: State, scope: ("all" | "pane" | "tab" | "window")?, anchor: PaneRef?) -> { Pane },
    -- id: 1-64 characters. Results are queued to the originating script instance.
    request: (id: string, effect: Effect) -> Effect,
    -- Result data: {text: string, truncated: boolean, available: boolean}; max 1 MiB.
    read_text: (pane: PaneRef, scope: ("screen" | "scrollback" | "last-output")?, max_bytes: number?) -> Effect,
    save_project: (pane: PaneRef, name: string, overwrite: boolean?) -> Effect,
    open_project: (name: string, newInstance: boolean?) -> Effect,
    list_projects: () -> Effect,
    move_pane: (pane: PaneRef, target: PaneRef?, placement: ("left" | "right" | "up" | "down" | "new-tab" | "new-window")?) -> Effect,
    detach_tab: (pane: PaneRef) -> Effect,
    timer: (name: string, delay_ms: number, repeat_ms: number?) -> Effect,
    cancel: (name: string) -> Effect,
    exec: (name: string, command: { string }, options: { cwd: string?, timeout_ms: number?, max_bytes: number? }?) -> Effect,
    read_file: (name: string, path: string, options: { timeout_ms: number?, max_bytes: number? }?) -> Effect,
    watch_file: (name: string, path: string) -> Effect,
    set_actions: (actions: { { id: string, title: string } }) -> Effect,
    select: (pane: PaneRef, title: string, items: { { id: string, title: string } }) -> Effect,
    prompt: (pane: PaneRef, title: string, text: string?) -> Effect,
    status: (pane: PaneRef, text: string?, tab_title: string?) -> Effect,
    override: (pane: PaneRef, values: { cursorBlink: boolean?, backgroundOpacity: number?, unfocusedDim: number? }) -> Effect,
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
    -- One effect, 1-8 unique channels, at most 128 panes. Defaults to anchor's tab.
    signals: (pane: PaneRef, values: { SignalValue }, scope: ("pane" | "tab" | "window")?) -> Effect,
    log: (message: string) -> Effect,
}
