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
-- Handlers run sandboxed with a short time limit and no direct access to files
-- or processes. They return an effect, a list of effects, or nil, and can wait
-- for an effect's result with afterglow.await. Actions appear
-- in the command palette and bind in Keybinds.bindings as
-- "script:<id>.<action>".
--
-- A helper that takes a pane targets the event's pane when the pane is nil.
-- In an event without a pane (on_load, on_config_changed, an action run with
-- no pane) such an effect fails: pass ev.terminal.active_pane instead.

export type LaunchOptions = { cwd: string?, env: {[string]: string | false}?, size: number? }

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

-- The pane overview: cards are the pane IDs of every tab in tab order while it is open, else the current tab's.
export type Overview = { open: boolean, highlighted: number, cards: { number } }
export type Window = { id: number, project: string, project_directory: string, focused: boolean, active_tab: number, active_pane: number, overview: Overview }
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
export type ClosedPaneEvent = { pane: { id: number, tab_id: number, window: number }, terminal: State }
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
-- A split also says which pane it split from and the edge they share: 1 left, 2 top, 3 right, 4 bottom.
export type PaneCreatedEvent = PaneEvent & { split_from: number?, edge: number? }
export type TabSwitchEvent = { pane: Pane, from_tab: number, to_tab: number, direction: number, terminal: State }
-- key: "a", "A", "Enter", "Escape", "Left", "F5"...; text: what it types, "" for control keys.
export type KeyEvent = { pane: Pane?, key: string, text: string, ctrl: boolean, shift: boolean, alt: boolean,
    meta: boolean, ["repeat"]: boolean, terminal: State }

-- curve: "linear", "ease-in-out", "spring" or a Phosphor curve spec such as "0.2,0,0,1" or "spring:20,1"
export type ShaderTransition = { duration_ms: number, curve: string? }
export type ShaderParameter = {
    id: string, name: string, type: string, default: (number | boolean | string),
    min: number?, max: number?, slot: number, unit: string, supported: boolean,
}
export type ShaderInstance = {
    instance: string, shader: string, layer: string,
    parameters: { ShaderParameter }, values: {[string]: number | boolean | string},
}
export type OperationError = { code: string, message: string }
export type SceneState = {
    tab_id: number, sample_ms: number, history_rows: number, history_normalized: number,
    audio_available: boolean, bass: number, mids: number, treble: number, overall: number,
}
export type SemanticPosition = {row: string, col: number} -- stable row identity, zero-based column
export type SemanticRegion = {
    id: string, kind: "prompt" | "input" | "output", start: SemanticPosition, finish: SemanticPosition,
    open: boolean, start_trimmed: boolean, available: boolean, exit_code: number?,
}
export type OperationData = {
    regions: {SemanticRegion}?,
    scene: SceneState?, presets: {{id: string, name: string, read_only: boolean}}?,
    shaders: { ShaderInstance }?,
    text: string?, truncated: boolean?, available: boolean?, name: string?,
    stdout: string?, stderr: string?, exit_code: number?, id: string?, title: string?,
    pane: Pane?, pane_id: number?, tab_id: number?, window_id: number?, panes: { number }?,
    -- Overview results.
    open: boolean?, highlighted: number?, cards: { number }?,
    -- Durable state results; revision must be supplied to the next state_write.
    value: {[string]: any}?, version: number?, revision: number?,
    -- Project listing and launch/restore results.
    names: { string }?, cwd: string?, reused: boolean?,
    pane_ids: {[string]: number}?, tab_ids: {[string]: number}?, warnings: { string }?,
}
export type ResultEvent = {
    request_id: string,
    kind: string,
    ok: boolean,
    data: OperationData,
    error: OperationError?,
    pane: (Pane | { id: number })?,
    -- Fresh at delivery; data describes the earlier operation. Absent from the
    -- busy result that afterglow.await gives when too many handlers wait.
    terminal: State,
}
export type SignalValue = { channel: number, value: number }

export type Effect = { kind: string, request_id: string? }
export type Effects = Effect | { Effect }

-- Handlers a script may define, all optional:
--   on_pane_closed(ev: ClosedPaneEvent)
--   on_pane_moved(ev: PaneEvent) -- same pane ID, new window/tab in the snapshot
--   on_focus(ev: PaneEvent)               on_bell(ev: PaneEvent)
--   on_title(ev: TitleEvent)              on_status(ev: StatusEvent)
--   on_command_started(ev: CommandStartedEvent)
--   on_command_finished(ev: CommandFinishedEvent)
--   on_user_var(ev: UserVarEvent)            on_context(ev: ContextEvent)
--   on_pane_created(ev: PaneCreatedEvent)    on_tab_switch(ev: TabSwitchEvent)
--   on_key(ev: KeyEvent) -- only while this script holds a key mode
--   run_action(action: string, ev: ActionEvent)
-- Packages receive on_load(ev: PackageLoadEvent); migrate(value, oldVersion, newVersion)
-- returns a new state object. Migration/init failures retain the old instance.
--   on_load(ev: ActionEvent)             on_unload(ev: ActionEvent)
--   on_config_changed(ev: ActionEvent)
--   on_timer(ev: ActionEvent & { name: string })
--   on_file_changed(ev: ActionEvent & { name: string, path: string, exists: boolean })
--   on_result(ev: ResultEvent) -- for effects with a request_id: afterglow.request, exec and read_file

export type PackageLoadEvent = {
    terminal: State,
    package: { id: string, version: string, api_version: number, home: string,
        config: {[string]: any}, assets: {[string]: string} },
    storage: { version: number, revision: number, value: {[string]: any} },
}
-- Package modules expose tables through local require("declared-module") or require("@dependency").

type PaneRef = Pane | number

declare afterglow: {
    panes: (state: State, scope: ("all" | "pane" | "tab" | "window")?, anchor: PaneRef?) -> { Pane },
    -- Applies the effect and waits in this handler: its data, or nil and the error,
    -- then the result event. Not in migrate, on_unload or code that runs at load time.
    await: (effect: Effect) -> (any?, OperationError?, ResultEvent),
    -- Applies 0-16 effects in order and waits for all: their result events, in order.
    await_all: (effects: { Effect }) -> { ResultEvent },
    -- Waits 0-600000 ms. Each sleep is a result: see the result limits.
    sleep: (ms: number) -> (any?, OperationError?, ResultEvent),
    -- id: 1-64 characters. Results are queued to the originating script instance.
    request: (id: string, effect: Effect) -> Effect,
    semantic_regions: (pane: PaneRef) -> Effect,
    -- Result data: {text: string, truncated: boolean, available: boolean}; max 1 MiB.
    read_region: (pane: PaneRef, region: string, max_bytes: number?) -> Effect,
    read_text: (pane: PaneRef, scope: ("screen" | "scrollback" | "last-output" | "selection")?, max_bytes: number?) -> Effect,
    -- Package-local JSON object; writes use the revision returned by state_read.
    state_read: () -> Effect,
    state_write: (value: {[string]: any}, revision: number) -> Effect,
    open_directory: (cwd: string, layout: string?) -> Effect,
    save_project: (pane: PaneRef, name: string, overwrite: boolean?) -> Effect,
    open_project: (name: string, newInstance: boolean?) -> Effect,
    list_projects: () -> Effect,
    -- left, right, up and down need a target; new-tab and new-window take none.
    move_pane: (pane: PaneRef, target: PaneRef?, placement: ("left" | "right" | "up" | "down" | "new-tab" | "new-window")?) -> Effect,
    detach_tab: (pane: PaneRef) -> Effect,
    timer: (name: string, delay_ms: number, repeat_ms: number?) -> Effect,
    cancel: (name: string) -> Effect,
    -- cwd is an absolute path; without one the process runs in Afterglow's working directory.
    exec: (name: string, command: { string }, options: { env: {[string]: string | false}?, cwd: string?, timeout_ms: number?, max_bytes: number? }?) -> Effect,
    -- path is absolute.
    read_file: (name: string, path: string, options: { timeout_ms: number?, max_bytes: number? }?) -> Effect,
    watch_file: (name: string, path: string) -> Effect,
    set_actions: (actions: { { id: string, title: string } }) -> Effect,
    select: (pane: PaneRef, title: string, items: { { id: string, title: string } }) -> Effect,
    prompt: (pane: PaneRef, title: string, text: string?) -> Effect,
    status: (pane: PaneRef, text: string?, tab_title: string?) -> Effect,
    override: (pane: PaneRef, values: { cursorBlink: boolean?, backgroundOpacity: number?, unfocusedDim: number? }) -> Effect,
    send_text: (pane: PaneRef, text: string) -> Effect,
    -- pane defaults to the event's pane; an event without one needs it given
    notify: (title: string, body: string?, pane: PaneRef?) -> Effect,
    split: (pane: PaneRef, direction: ("right" | "down" | "left" | "up")?, command: (string | { string })?, options: LaunchOptions?) -> Effect,
    new_tab: (pane: PaneRef, cwd: string?, command: (string | { string })?, options: LaunchOptions?) -> Effect,
    new_window: (cwd: string?, command: (string | { string })?, options: LaunchOptions?) -> Effect,
    focus: (pane: PaneRef) -> Effect,
    close: (pane: PaneRef) -> Effect,
    set_title: (pane: PaneRef, title: string) -> Effect, -- "" returns to the program's title
    copy: (text: string) -> Effect,
    open_url: (url: string) -> Effect, -- http, https and mailto
    action: (pane: PaneRef, name: string) -> Effect, -- a keybind action: split_right, new_tab...
    -- Query active instances, metadata and effective values via on_result.
    shaders: (pane: PaneRef) -> Effect,
    scene_state: (pane: PaneRef) -> Effect,
    shader_presets: (pane: PaneRef, instance: string) -> Effect,
    shader_preset: (pane: PaneRef, instance: string, preset: string, scope: ("pane" | "tab" | "window")?, transition: ShaderTransition?) -> Effect,
    save_shader_preset: (pane: PaneRef, instance: string, name: string) -> Effect,
    -- Replaces this script's overrides; {} resets. Window effects require window scope.
    shader_parameters: (pane: PaneRef, instance: string, values: {[string]: number | boolean | string}, scope: ("pane" | "tab" | "window")?, transition: ShaderTransition?) -> Effect,
    -- sets uSignal[channel] (0-7) on the pane's shaders: the value, and its age from now
    signal: (pane: PaneRef, channel: number, value: number?) -> Effect,
    -- One effect, 1-8 unique channels, at most 128 panes. Defaults to anchor's tab.
    signals: (pane: PaneRef, values: { SignalValue }, scope: ("pane" | "tab" | "window")?) -> Effect,
    log: (message: string) -> Effect,
    -- The window's keys go to on_key until end_key_mode, a handler error or unload.
    key_mode: (pane: PaneRef, label: string?) -> Effect,
    end_key_mode: () -> Effect,
    -- Result data: the Overview.
    overview: (pane: PaneRef, operation: ("open" | "close" | "choose")?) -> Effect,
    overview_move: (pane: PaneRef, step: number?, target: PaneRef?) -> Effect,
    -- Plays over the pane in Shaders.paneEffects. color is a palette index (default 4).
    pulse: (pane: PaneRef, effect: ("portal" | "flash" | "shake" | string | number), options: { duration_ms: number?, color: number? }?) -> Effect,
}
