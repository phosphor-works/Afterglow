-- SPDX-FileCopyrightText: 2026 fuddlesworth
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Type definitions for afterglow scripts, for editors with Luau support.
--
-- A script is a .luau file that returns a module table, in a scripts
-- directory: scripts/ beside the configuration file (~/.config/afterglow/scripts/),
-- ~/.local/share/afterglow/scripts/, or afterglow/scripts/ in a system data
-- directory such as /usr/share. Of two scripts with one id, the one in the
-- directory earlier in this list loads:
--
--   local script = {}
--   script.metadata = {
--       id = "my-script",              -- optional, defaults to the file name
--       name = "My Script",            -- optional: the name in afterglow ctl scripts, defaults to the id
--       commands = { hello = { summary = "Say hello", usage = "[NAME]", palette = "Say Hello" } },
--   }
--   function script.on_command_finished(ev: CommandFinishedEvent): Effects? ... end
--   function script.run_command(command: string, ev: CommandEvent): Effects? ... end
--   return script
--
-- Handlers run sandboxed with a short time limit and no direct access to files
-- or processes. They return an effect, a list of effects, or nil, and can wait
-- for an effect's result with afterglow.await. Commands run from the shell
-- (`aglow run <id> <command>`), from the command palette and the context
-- menu when they have a palette title, from a key as "script:<id>.<command>"
-- (Keybinds.bindings, a key table or a key sequence), and from
-- `afterglow ctl action script:<id>.<command>` or a SCRIPT.COMMAND call of
-- the commands target.
-- A package declares its commands in package.json, not in metadata.
--
-- A helper that takes a pane targets the event's pane when the pane is nil.
-- In an event without a pane (on_load, on_config_changed) such an effect fails: pass ev.terminal.active_pane instead.

export type LaunchOptions = { cwd: string?, env: {[string]: string | false}?, size: number? }

export type Pane = {
    id: number,
    window: number,
    tab: number,
    tab_id: number, -- stable for the process lifetime; tab is the zero-based position
    title: string,
    cwd: string,
    command: string, -- running now (OSC 133), "" at the prompt
    cols: number, -- the text's cells, without the pane's docks
    rows: number,
    focused: boolean,
    context: string, -- where the shell runs when not here as you ("root", "deploy@prod-1"), else ""
}

-- The pane overview: cards are the pane IDs of every tab in tab order while it is open, else the current tab's.
export type Overview = { open: boolean, highlighted: number, cards: { number } }
-- cols and rows: the cells that fit in the window. A top or bottom window dock is cols wide.
export type Window = { id: number, project: string, project_directory: string, focused: boolean, active_tab: number,
    active_pane: number, overview: Overview, cols: number, rows: number }
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
-- The commands that a pane's shell runs (shell integration), not script commands.
export type CommandStartedEvent = { pane: Pane, command: string, terminal: State }
export type CommandFinishedEvent = {
    pane: Pane,
    command: string,
    exit_code: number, -- -1 when the shell did not report one
    duration_ms: number,
    terminal: State,
}
export type StatusEvent = { pane: Pane, state: string, text: string, terminal: State }
export type HostEvent = { pane: Pane?, terminal: State } -- the targeted pane, if any
-- A command run. A run from the palette, a key, the menu or `afterglow ctl action` has
-- no arguments. It ends when the handler returns, also after it waited.
export type CommandEvent = {
    args: { string }, -- at most 256, each at most 4096 UTF-16 units
    -- The client's directory, else the pane's, else "". For source "remote", a directory on the
    -- remote host, as text: not a local path.
    cwd: string,
    pane: (Pane | { id: number })?, -- only the id when the pane closed before the run started
    -- All of the client's stdin, for a command with stdin = true run from the shell. nil for
    -- another source, and when aglow run reads none: with -n, or when stdin is a terminal.
    -- A run with more than Scripts.commandStdinLimit does not start, and aglow run refuses
    -- stdin that is not UTF-8 text (exit status 2).
    stdin: string?,
    source: "shell" | "palette" | "key" | "menu" | "control" | "remote",
    remote: { host: string }?, -- for source "remote": the host of the ssh session, as afterglow ssh named it
    run: string, -- for afterglow.output, output_error and exit
    terminal: State,
}
export type UserVarEvent = { pane: Pane, name: string, value: string, terminal: State } -- iTerm2 SetUserVar
export type ContextEvent = { pane: Pane, context: string, terminal: State } -- OSC 3008 / RemoteHost; "" back home
-- A split also says which pane it split from and the edge they share: 1 left, 2 top, 3 right, 4 bottom.
export type PaneCreatedEvent = PaneEvent & { split_from: number?, edge: number? }
-- A palette index (0-255, the theme's 0-15 included) or "#rrggbb". Absent: the theme's
-- default color, except that a run without bg takes the dock's fill.
export type DockColor = number | string
export type DockRun = { text: string?, fg: DockColor?, bg: DockColor?, bold: boolean?, italic: boolean?,
    underline: boolean?,
    item: string?, -- 1-64 characters, no control characters; names the run's cells in on_dock_click, on_dock_hover, on_dock_wheel and on_dock_drag
    spacer: boolean? } -- no text: takes the cells the other runs of its line leave, shared among the spacers
-- A field that is not listed here refuses the dock.
export type DockSpec = {
    id: string, -- 1 to 32 ASCII letters, digits, - or _
    edge: "top" | "bottom" | "left" | "right",
    size: number?, -- rows (top, bottom: 1-8) or columns (left, right: 1-80); default 1
    tab_bar: boolean?, -- top, window scope only: drawn in place of the tab bar; default false
    -- "window" (default): at the window's edge, the panes get what is left. "pane": at the pane's
    -- edge, part of the pane (its shaders, overview card, transitions); its text gets what is left.
    scope: ("window" | "pane")?,
    fill: DockColor?, -- the background of the whole dock; default the theme's background
    lines: { { DockRun } }?, -- top, bottom: at most size lines; left, right: at most 256
}
-- Every dock event below has dock, scope and window. scope: the dock's scope. pane: a pane
-- dock's pane, or the window's focused pane for a window dock.
export type DockClickEvent = { pane: Pane?, dock: string, scope: "window" | "pane", item: string, button: "left" | "middle" | "right",
    window: number, terminal: State }
-- item: the item under the pointer; nil when the pointer left the dock's items.
export type DockHoverEvent = { pane: Pane?, dock: string, scope: "window" | "pane", item: string?, window: number, terminal: State }
-- steps: whole wheel notches, at least 1. item: the item under the pointer, if any.
export type DockWheelEvent = { pane: Pane?, dock: string, scope: "window" | "pane", item: string?, direction: "up" | "down" | "left" | "right",
    steps: number, window: number, terminal: State }
-- A left-button drag that began on item. phase: "start" once the pointer has moved the drag
-- distance, "move" when a pointer move changes over or the cell, "end" at the release, "cancel"
-- when the drag was cut off. Moves count toward the callback limit. over: the item under the
-- pointer, if any. col, row: the cell under the pointer, from 0 at the dock's top left; past the
-- dock's edges they are below 0, or its size or more.
export type DockDragEvent = { pane: Pane?, dock: string, scope: "window" | "pane", item: string, phase: "start" | "move" | "end" | "cancel",
    over: string?, col: number, row: number, window: number, terminal: State }
-- Where the dock shows: cols and rows are its cells, 0 when it does not show.
export type DockLayoutEvent = { pane: Pane?, dock: string, scope: "window" | "pane", shown: boolean, cols: number, rows: number, window: number,
    terminal: State }
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
-- A key that is not listed here refuses the overlay.
export type OverlayOptions = {
    -- At most 4 MiB of UTF-8. Without it, the program reads the terminal.
    stdin: string?,
    -- Where stdout of the result comes from: the program's stdout (the default), the
    -- file that $AFTERGLOW_RESULT names, or nowhere.
    capture: ("stdout" | "file" | "none")?,
    -- Absolute, "~" or "~/...". A directory that does not exist here starts it in $HOME.
    cwd: string?,
    -- At most 256 variables and 64 KiB of names and values.
    env: {[string]: string | false}?,
    -- Shows as the pane's title while the overlay runs, and in the top edge of a border (cut
    -- to its width), at most 512 characters, with no control character, line break or
    -- bidirectional control.
    title: string?,
    -- A popup over the pane, which stays visible around it: a whole number of cells, 1-10000,
    -- or a percentage of the pane's grid, "1%"-"100%". The border counts. Unset: the whole grid.
    width: (number | string)?,
    height: (number | string)?,
    -- Where a popup sits in the pane. The default is "center". No effect without a size.
    position: ("center" | "top" | "bottom" | "left" | "right" | "top-left" | "top-right" | "bottom-left" | "bottom-right")?,
    -- A line around the program's grid, in the popup border color. The default is true for
    -- a popup and false for a full-pane overlay. true without a size: a popup that fills the pane.
    border: boolean?,
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
    terminal: State?,
}
export type SignalValue = { channel: number, value: number }

export type Effect = { kind: string, request_id: string? }
-- A command's fields, in metadata.commands, package.json and set_commands.
-- remote: aglow run on a remote host can run it, through afterglow ssh (Terminal.sshRemoteCommands).
export type CommandSpec = { summary: string, usage: string?, palette: string?, stdin: boolean?, remote: boolean? }
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
--   on_dock_click(ev: DockClickEvent) -- a click on an item of this script's dock
--   on_dock_hover(ev: DockHoverEvent) -- the pointer moved onto an item of this script's dock, or off its items
--   on_dock_wheel(ev: DockWheelEvent) -- the mouse wheel over this script's dock
--   on_dock_drag(ev: DockDragEvent) -- a drag that began on an item of this script's dock
--   on_dock_layout(ev: DockLayoutEvent) -- this script's dock showed, went or changed its size
--   run_command(command: string, ev: CommandEvent)
-- Packages receive on_load(ev: PackageLoadEvent); migrate(value, oldVersion, newVersion)
-- returns a new state object. Migration/init failures retain the old instance.
--   on_load(ev: HostEvent)             on_unload(ev: HostEvent)
--   on_config_changed(ev: HostEvent)
--   on_timer(ev: HostEvent & { name: string })
--   on_file_changed(ev: HostEvent & { name: string, path: string, exists: boolean })
--   on_result(ev: ResultEvent) -- for effects with a request_id: afterglow.request, exec and read_file

export type PackageLoadEvent = {
    terminal: State,
    package: { id: string, version: string, api_version: number, home: string,
        config: {[string]: any}, assets: {[string]: string} },
    storage: { version: number, revision: number, value: {[string]: any} },
}
-- Package modules expose tables through local require("declared-module") or require("@dependency").

-- A pane record or its id. nil: the event's pane (see the top of this file).
type PaneRef = Pane | { id: number } | number | nil

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
    -- A command string is the program alone, with no arguments: it is not split and no shell runs it.
    -- cwd is an absolute path; without one the process runs in Afterglow's working directory.
    exec: (name: string, command: string | { string }, options: { env: {[string]: string | false}?, cwd: string?, timeout_ms: number?, max_bytes: number? }?) -> Effect,
    -- path is absolute.
    read_file: (name: string, path: string, options: { timeout_ms: number?, max_bytes: number? }?) -> Effect,
    watch_file: (name: string, path: string) -> Effect,
    -- Commands added at runtime, beside the manifest's, by name. Replaces the commands of the last set_commands.
    -- A name that the manifest has is refused.
    set_commands: (commands: {[string]: CommandSpec}) -> Effect,
    -- To the stdout and stderr of `aglow run`, within Scripts.commandOutputLimit. A run
    -- with no client drops stdout and shows stderr in a notification.
    output: (run: string | CommandEvent, text: string) -> Effect,
    output_error: (run: string | CommandEvent, text: string) -> Effect,
    -- The run's exit status, 0-255. A run that sets none exits with 0.
    exit: (run: string | CommandEvent, code: number) -> Effect,
    -- An item without a title shows its id.
    select: (pane: PaneRef, title: string, items: { { id: string, title: string? } }) -> Effect,
    prompt: (pane: PaneRef, title: string, text: string?) -> Effect,
    -- Runs command over the pane, in its place or as a popup over a part of it, and with its
    -- input, until the program exits.
    -- Result data: {exit_code: number, stdout: string, truncated: boolean}. exit_code is -1
    -- when the status is unknown. stdout holds at most 1 MiB of the program's bytes.
    -- Errors: invalid_effect, rate_limited, no_pane, busy, spawn_failed, cancelled (no data).
    -- The overlay takes the focus: of several at once, the last one opened has it.
    overlay: (pane: PaneRef, command: string | { string }, options: OverlayOptions?) -> Effect,
    status: (pane: PaneRef, text: string?, tab_title: string?) -> Effect,
    override: (pane: PaneRef, values: { cursorBlink: boolean?, backgroundOpacity: number?, unfocusedDim: number? }) -> Effect,
    -- Lines of styled text at an edge of the pane's window, or of the pane (spec.scope). Sending one id again replaces it.
    dock: (pane: PaneRef, spec: DockSpec) -> Effect,
    undock: (pane: PaneRef, id: string, scope: ("window" | "pane")?) -> Effect,
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
    -- A keybind action: split_right, new_tab... Not a script command (script:<id>.<command>).
    -- Fails while a prompt or a script dialog is open in the window.
    action: (pane: PaneRef, name: string) -> Effect,
    -- The tab menu of the pane's tab: where the click on a dock item before it was, else at the pointer.
    -- busy while a prompt, a script dialog or the command palette is open in the window.
    tab_menu: (pane: PaneRef) -> Effect,
    -- Moves the pane's tab to index (Tab.index, from 0) in its window, without showing it.
    move_tab: (pane: PaneRef, index: number) -> Effect,
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
