<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The mux daemon

This document describes a design. The mux is not implemented yet.

A mux keeps a terminal session running when its window closes.
A session is one set of windows, tabs and panes with running processes.
Open a new window later to see the same session.
Open the same session from another display, or from another computer over SSH.
The mux is opt-in. A normal launch does not use it.

Local sessions come first.
[Remote hosts](#remote-hosts) use the same daemon and the same protocol, over an SSH connection.

## Sessions and the daemon

The mux has two parts.

| Part | Role |
| --- | --- |
| Daemon | `afterglow-mux`, one headless process with no Qt. It owns the sessions, the PTYs, the terminal state and the layout. |
| Client | The Afterglow GUI that shows a session and sends input. |

The daemon owns the window, tab and pane layout.
The client shows the layout. It does not own it.
Closing the client does not end a detached session.

The daemon uses only `libs/afterglow-vt`, `libs/afterglow-pty`, `libs/afterglow-proto` and `phosphor-channel`.
It needs no Qt, no display and no GPU.
The build that builds `aglow` alone also builds `afterglow-mux`, so a remote host can install it from a package.
That build then also needs the 32-bit PCRE2 library, because `afterglow-vt` uses it for search.

The first mux command starts the daemon when it does not run.
The daemon runs in its own systemd user unit, `afterglow-mux.service`.
It does not run in the unit of the client.
The reason: when systemd-oomd stops the unit of the client, the sessions must stay.
Without a systemd user manager, the daemon starts in a new session with `setsid`.

The daemon exits when it has no sessions.
It also stops when the systemd user manager stops, which is normally at the last logout.
A mux session and a normal `afterglow` launch can run at the same time.

## Commands

| Command | Function |
| --- | --- |
| `afterglow mux server` | Runs the daemon in the foreground. The user unit uses this command. |
| `afterglow mux new` | Creates a session and opens it. Options: `--name`, `--cwd`, `--detach`, `--`. |
| `afterglow mux attach [NAME]` | Opens a client on a session. Option: `--ssh DESTINATION` (see [remote hosts](#remote-hosts)). |
| `afterglow mux detach [NAME]` | Detaches the client of a session and keeps the session. |
| `afterglow mux ls` | Lists the sessions, with their state (attached or detached) and their windows. |
| `afterglow mux kill NAME` | Ends a session and its processes. |

`afterglow mux new --detach` creates a session without opening a client.
`afterglow mux new` without `--name` gives the session the smallest free number as its name, from `1`.
`afterglow mux attach` without a name opens the session that was attached last.
When no session exists, it creates one.
In a pane of a session, `afterglow mux detach` without a name detaches that session.
`afterglow mux kill` asks before it ends running programs, as `Terminal.confirmClose` does.

## Which process and pane

| Item | Value |
| --- | --- |
| Daemon socket | `$XDG_RUNTIME_DIR/afterglow-mux.sock`. It does not depend on the display. |
| `AFTERGLOW_SOCKET` in a session pane | The daemon socket |
| `AFTERGLOW_PANE_ID` in a session pane | The pane ID in the daemon. It stays the same when another client attaches. |
| `AFTERGLOW_SESSION` in a session pane | The session name |

The daemon refuses a connection from a process of another user (`SO_PEERCRED`).
Tests set their own `XDG_RUNTIME_DIR`, so a test daemon never reaches the daemon of the user.

The client is the main process of the display (see [launches](CONTROL.md#launches)).
`afterglow mux attach` hands the attach to that process, as a launch hands a new window.
All windows on one display then share one GPU device.

## Control

`afterglow ctl` targets a normal window by default.
`afterglow ctl --session NAME ...` targets a session in the daemon.
In a session pane, `afterglow ctl` targets the daemon through `AFTERGLOW_SOCKET`.
Daemon commands work while nothing is attached.

The daemon serves the phosphor-ipc wire format without Qt.
`libs/afterglow-proto` has the client side of this format, and gets the server side too.

| Call | While attached | While detached |
| --- | --- | --- |
| Layout: new tab, split, close, move | The daemon changes the layout. The client shows the change. | The daemon changes the layout. |
| Pane input and pane text | The daemon | The daemon |
| Window and chrome: overview, palette, focus | The daemon passes the call to the client. | Refused with an error that says that no client is attached |

Scripts run in the client.
While a session is detached, no script runs for it.
`afterglow ctl` still controls it.

## Close, detach and disconnect

| Event | Result |
| --- | --- |
| The last window of a session closes | `Mux.closeOnLastWindow`: `kill` ends the session, `detach` keeps it |
| The `detach_session` action | The client closes the windows of the session and keeps the session |
| The client exits without a close: a crash, a signal, a compositor restart, a lost SSH connection | The session detaches and keeps running |
| A pane or a tab closes | Its processes end, as in a normal window |

The default of `Mux.closeOnLastWindow` is `kill`.
This keeps the current behavior of the close button.
Only a close that the user asks for can end a session.
A client that disappears always detaches.

## One client at a time

One client attaches to a session at a time.
Attaching from a second client takes over the session.
The first client detaches and reports the takeover.
`--no-takeover` makes the second attach refuse instead.
Showing one session on two clients at once is not part of this design.

The client measures each pane in cells with its own font, and sends the sizes to the daemon.
The daemon then resizes the PTYs.
After a takeover from a display with another font or scale, the panes reflow to the new sizes.
A detached session keeps the sizes of its last client.

## Detached sessions

A detached session keeps running.
The daemon keeps reading PTY output and parses it into the terminal.
Scrollback grows while the session is detached.
The `Terminal.scrollback` limit still applies.
The processes do not pause.

| Event while detached | Result |
| --- | --- |
| Title and working directory | The daemon keeps the last value. The next client shows it. |
| Bell, notification, clipboard write | The daemon keeps a count of each for the next client, and drops the content |
| Clipboard read, file transfer, drag and drop | Refused, as when the user denies the request |
| Queries that the terminal answers itself (DA1, DSR, color queries) | Answered by the daemon |

## Where each part runs

The daemon owns everything that the program in the pane can see.
The client owns everything that the user sees and touches.

| Part | Runs in | Notes |
| --- | --- | --- |
| Parser, screen, scrollback, reflow | Daemon | |
| Answers to the program: DA1, DSR, color queries, OSC 24569 | Daemon | The client sends its theme colors when it attaches |
| Key, mouse and paste encoding | Daemon | The client sends key and mouse events. The daemon encodes them with the modes of the pane. |
| Search, and the text of a selection | Daemon | The client sends the query or the range and gets the result |
| Image decoding and the image store | Daemon | It decodes with stb_image, as `aglow` does. The client gets each image once, by ID. |
| Clipboard, notifications, bell, title, OSC 133 marks, user variables | Daemon parses, client acts | Each one is an event on the output channel |
| File transfer and drag and drop | Daemon parses, client acts | The client prompts and reads or writes the local files |
| Rendering, fonts, shader packs, chrome, accessibility | Client | From the state that the daemon sends |
| Scripts, packages, package commands | Client | |
| Remote sockets of `afterglow ssh` | Daemon | See [package commands from remote hosts](#package-commands-from-remote-hosts) |

The daemon reads no configuration file.
`afterglow mux new` and each attach send the values that the daemon needs: `Terminal.scrollback`, `Terminal.term`, the theme colors and the remote command settings.
A session keeps the values of its last client.

## Projects and sessions

A project stores a launch layout. Opening it starts new processes.
A session holds running processes.
A project can open into a new session.
A session can save its structure as a project.
See [projects](PROJECTS.md) for the project format.

## Architecture

The daemon owns the terminal state, the PTY and the layout.
The client owns rendering and input.
Two planes connect the daemon and the client.

| Plane | Library | Wire | Role |
| --- | --- | --- | --- |
| Control | The phosphor-ipc wire format | JSON lines | Commands, layout and metadata |
| Data | phosphor-channel | Byte frames | Terminal state and input |

On one computer, the control plane is the daemon socket.
It negotiates the data channels and passes a rendezvous path in `$XDG_RUNTIME_DIR`.
The data channels open their own Unix socket at that path and exchange file descriptors there.
The rendezvous socket has mode 0600 and checks the peer with `SO_PEERCRED`.

Over SSH, one byte stream carries both planes (see [remote hosts](#remote-hosts)).

This division matters. Terminal state changes at a high rate.
JSON is wrong for it. The data plane uses binary frames.

## Terminal state

The output channel carries the state of the screen, not the bytes from the PTY.
The daemon parses the PTY output. The client never parses it.

| Frame | Content |
| --- | --- |
| Snapshot | The full visible screen, the cursor, the modes that the client needs and the recent scrollback. Sent when a client attaches. |
| Damage | The rows that changed since the last frame that the client acknowledged |
| Scrollback page | Older lines, sent when the client asks for them (scrolling, search results) |
| Image | Decoded image data with its ID. Sent once for each image. |
| Event | Title, working directory, bell, notification, clipboard, OSC 133 mark, transfer and drag and drop messages |

A damage frame always describes the newest state.
So the daemon can merge the damage of several frames into one.
A slow client sees fewer frames, not a growing queue.
Events are never merged or dropped while a client is attached.

The codec tag of these frames is `afterglow.state.v1`.
`libs/afterglow-proto` writes and reads it, so the daemon and the client use the same code.

## The data channel

`phosphor-channel` is a new data-only library in the PlasmaZones checkout. It uses no Qt and no JSON.
It moves bytes between two processes, in order, with backpressure.
It is a separate library from phosphor-ipc.

A channel is unidirectional. One writer feeds one reader.
A session uses two channels.
One carries terminal state from the daemon to the client.
The other carries input from the client to the daemon.

Each channel carries frames of opaque bytes and a codec tag.
The codec tag names the payload format, such as `afterglow.state.v1`.
The library stores the codec tag. It does not parse the payload.

Backpressure is explicit.
A write returns `would-block` when the channel is full.
The writer then merges its frames. A blocking write is also available.
The input channel uses the blocking write, because input is never dropped.

### What the daemon needs from phosphor-channel

| Need | Reason |
| --- | --- |
| No Qt, and a build that needs none of the other Phosphor libraries | The daemon has no Qt. A remote host builds it with the Qt-free build. |
| A stream transport over any connected file descriptor | Over SSH, the channels go through the standard input and output of a process |
| A handshake on that transport that passes no file descriptors | SSH cannot pass file descriptors |
| Several channels on one stream, each with its own backpressure | One SSH stream carries control, state and input. A large output must not hold back a keystroke. |
| The shared-memory ring as a transport on one computer | The performance path |

### Transports

| Transport | Use | Cost |
| --- | --- | --- |
| Shared-memory ring | Terminal state on one computer, the performance path | One copy per byte |
| Unix socket frames | Input, and the fallback on one computer | Two copies per byte, into the kernel and out |
| Stream frames | All channels over SSH | Two copies per byte, plus SSH |

The shared-memory ring is a memfd mapped by both processes.
It has the seals `F_SEAL_SHRINK`, `F_SEAL_GROW` and `F_SEAL_SEAL`, so its size cannot change.
It cannot be sealed against writes, because both ends write their positions.
A header holds write and read positions on separate cache lines.
The writer writes whole frames. A skip marker pads the wrap.
The reader wakes on an eventfd and drains the ring.
The reader checks each length and position that it reads, because the other process can change the ring at any time.

The channel handshake sends the memfd and eventfd over a raw Unix socket.
It does not send them through phosphor-ipc. QLocalSocket does not pass file descriptors cleanly.

### Versioning

The channel protocol version covers the handshake, the framing and the ring layout.
Two ends refuse a version mismatch, with a message that names both versions.
The codec tag and its version cover the payload.
Payload versioning lives in the consumer, not in the channel.

## Remote hosts

A client can attach to a daemon on another computer over SSH.
The daemon runs on the remote host. The client runs on your computer.
Your scripts, shader packs, fonts and clipboard stay on your computer.

### Requirements on the remote host

- `afterglow-mux`, from a package or from the Qt-free build (see [build aglow alone](BUILD.md#build-aglow-alone)).
- An SSH server that runs commands for you. Port forwarding is not necessary.
- On a host that stops your processes at logout, lingering for your user (`loginctl enable-linger`). Without it, detached sessions end when your last login ends.

Afterglow does not copy `afterglow-mux` to a remote host, for the same reasons as `aglow` (see [remote hosts](DESKTOP.md#remote-hosts)).

### Attach

`afterglow mux attach --ssh DESTINATION [NAME]` does these steps:

1. It opens the shared connection of `afterglow ssh` to the destination, or uses the one that is open.
2. It runs the probe of `afterglow ssh` over that connection. The probe also reports `afterglow-mux --version`.
3. It runs `afterglow-mux proxy` on the remote host over the shared connection.
4. The proxy starts the remote daemon when it does not run, and connects to the daemon socket.
5. The proxy carries the stream on its standard input and output.

You authenticate once, for the shared connection.
The attach uses no `-L` or `-R` forward, so `AllowStreamLocalForwarding` on the server does not matter.

| Probe result | Action |
| --- | --- |
| `afterglow-mux` found, same protocol version | The attach continues |
| `afterglow-mux` missing | The attach stops with a message that names the host and the missing command |
| `afterglow-mux` found, other protocol version | The attach stops with a message that names the two versions |

### The stream

One stream carries the control plane and the data channels, with the stream transport of phosphor-channel.
The control plane is one channel pair on the stream. It carries the phosphor-ipc JSON lines unchanged.
The shared-memory ring is not used over SSH.
Frames on the stream can be compressed with zlib. The codec tag records it.

The daemon sends damage at the rate at which the client acknowledges frames.
On a slow link the client shows fewer frames, and the daemon never blocks on the network.

A lost SSH connection detaches the session.
`afterglow mux attach --ssh` again continues it.

### Terminfo on the remote host

A remote session uses the `Terminal.term` value of the client.
When the value is `afterglow`, the attach installs the terminfo entry over the shared connection, as `afterglow ssh` does (see [SSH](INPUT.md#ssh)).
When the host has no `tic`, the session uses `xterm-256color`.

## Package commands from remote hosts

A package command that declares `"remote": true` can run from a remote host.
`Terminal.sshRemoteCommands` turns this on, and is off by default.
The mux changes how a run reaches the client. It does not change which commands can run.

### In a session on a remote host

In a pane of a remote daemon, `AFTERGLOW_SOCKET` names the remote daemon socket.
`aglow run` connects to it directly.
The daemon passes calls of the `remote` target to the attached client over the stream.
The client applies `Terminal.sshRemoteCommands` and the `"remote": true` rule.
The handler gets `ev.source = "remote"` and `ev.remote.host`.

This path needs no `-R` forward and no OSC 24569 query.
While the session is detached, the daemon refuses the call with an error that says that no client is attached.

### `afterglow ssh` in a local session

A pane of a local session can run `afterglow ssh`.
Its terminal is in the daemon, so the daemon answers the OSC 24569 query of `aglow run` on the remote host.
The daemon also owns the remote socket of the SSH session:

1. `afterglow ssh` sends `openRemoteSocket` to the daemon through `AFTERGLOW_SOCKET`.
2. The daemon opens the local socket, watches the `afterglow ssh` process and runs `ssh -O cancel` when it exits.
3. The daemon passes calls on that socket to the attached client, as for a remote daemon.

The forward then stays valid when the client detaches and another client attaches.
While the session is detached, the daemon refuses the calls.

## Performance

The shared-memory ring copies each byte once. A socket copies it twice.
The client drains the ring at its frame rate.
This is the same throttle the renderer uses today.
The daemon never blocks. It merges damage when the channel is full.
Input has its own channel. A flood does not starve a keystroke.

## Limits and settings

| Item | Value |
| --- | --- |
| Clients per session | 1 |
| Ring capacity | `Mux.ringCapacity`. Default 8 MiB. |

The `Terminal.scrollback` limit from the [configuration reference](CONFIGURATION.md) still applies.

| Setting | Default | Meaning |
| --- | --- | --- |
| `Mux.closeOnLastWindow` | `kill` | `kill` ends the session. `detach` keeps it. |
| `Mux.ringCapacity` | `8` | The size of the shared-memory ring for terminal state, in MiB |

## Deferred

| Item | Status |
| --- | --- |
| Mirroring one session to two clients | Deferred |
| Local echo before the remote host answers, as mosh does | Deferred |
| A connection that survives a change of network address | Deferred. A lost SSH connection detaches the session. |
