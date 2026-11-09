// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/// Where the sockets are. The terminal listens on its socket (InstanceSocket)
/// and the mux daemon on its own (muxSockets()). Their clients (`afterglow
/// ctl`, `aglow run`, `afterglow mux`) look the paths up here, so that all of
/// them agree on them.
namespace AfterglowProto {

/// Reads an environment variable: nullptr when it is not set. Tests give
/// their own.
using EnvironmentLookup = std::function<const char*(const char* name)>;
/// The process's environment.
const char* systemEnvironment(const char* name);

/// The longest socket path: sockaddr_un holds 108 bytes, the NUL included.
inline constexpr std::size_t MaxSocketPathBytes = 107;

/// Why @p path cannot be the address of a socket: it is empty, or over
/// MaxSocketPathBytes. Empty when it can.
std::string socketPathError(std::string_view path);

/// The Wayland display's name, safe in a file name: a leading "/" goes, and
/// each byte that is not a letter, a digit, "-", "_" or "." becomes "_".
/// "wayland-0" when it is empty.
std::string displayName(std::string_view waylandDisplay);

/// $XDG_RUNTIME_DIR/afterglow-<display>.sock: the socket of the main
/// afterglow on the display. <display> is displayName() of WAYLAND_DISPLAY,
/// after a leading "$XDG_RUNTIME_DIR/": Wayland reads a relative display in
/// that directory, so both spellings name one compositor. Empty when
/// XDG_RUNTIME_DIR is not set or not an absolute path.
std::string sharedSocketPath(const EnvironmentLookup& env = systemEnvironment);
/// $XDG_RUNTIME_DIR/afterglow-<display>-<pid>.sock: the socket of an
/// afterglow that runs on its own. That is one started with --config, -o or
/// --class, and one that could not hand its window to the main afterglow
/// (another executable, or no answer). Empty as sharedSocketPath() is.
std::string ownSocketPath(std::int64_t pid, const EnvironmentLookup& env = systemEnvironment);

struct SocketLookup
{
    std::string path; ///< empty when there is none: error says why
    /// The socket of $AFTERGLOW_SOCKET. A pane of a window sets it to the
    /// socket of the Afterglow that runs the pane. A pane of a mux session
    /// sets it to the socket of the mux daemon, which answers the `control`
    /// and `commands` targets with CALL_FAILED until a window shows the
    /// session.
    bool own = false;
    /// $AFTERGLOW_PANE_ID whenever $AFTERGLOW_SOCKET is set, else 0. The
    /// lookup cannot tell a socket of another Afterglow from the pane's own:
    /// a user who points AFTERGLOW_SOCKET at another one clears
    /// AFTERGLOW_PANE_ID, or gives the client a pane.
    std::int64_t pane = 0;
    std::string error;
};

/// The socket of a client: $AFTERGLOW_SOCKET, the Afterglow that runs this
/// pane, else the shared socket of the display. A pane ID without a socket
/// is an error: another afterglow on the display numbers its panes from 1
/// too, and would act on a pane with the same number. So is a path over
/// MaxSocketPathBytes.
SocketLookup findSocket(const EnvironmentLookup& env = systemEnvironment);

/// A socket of the mux daemon.
struct MuxSocket
{
    std::string path;
    /// The directory of the socket, which the daemon creates with mode 0700
    /// and checks before it listens. It holds the sockets and lock files of
    /// the mux daemon only: in the state directory, those of every computer
    /// that shares the home directory.
    std::string directory;
};

/// The sockets of the mux daemon, in the order that a client tries them. The
/// daemon listens on each of them. They do not depend on the display:
///
///   $XDG_RUNTIME_DIR/afterglow-mux/socket
///   $XDG_STATE_HOME/afterglow-mux/<muxStateSocketName()> (by default under ~/.local/state)
///
/// The second outlives the runtime directory, which logind removes when the
/// user's last login ends, and a daemon started without XDG_RUNTIME_DIR is on
/// it too. Its name is of this computer: computers that share a home
/// directory over NFS get files of their own.
struct MuxSockets
{
    std::vector<MuxSocket> candidates;
    /// Why each candidate that is not in the list was left out. The lookup
    /// failed when candidates is empty.
    std::string error;
};

MuxSockets muxSockets(const EnvironmentLookup& env = systemEnvironment);

/// The file name of the state socket, from the contents of /etc/machine-id
/// and the host name: 32 hex digits of a keyed hash of the machine ID and
/// ".sock", when the trimmed ID is 32 hex digits. Else hostFileName(@p host)
/// and ".sock". The machine ID, not the host name, so that a new host name
/// does not move the socket away from a running daemon.
std::string muxStateSocketName(std::string_view machineId, std::string_view host);
/// muxStateSocketName() of this computer.
std::string muxStateSocketName();

/// @p host, safe as a file name: each "/" becomes "_", and an empty one is
/// "localhost".
std::string hostFileName(std::string_view host);

} // namespace AfterglowProto
