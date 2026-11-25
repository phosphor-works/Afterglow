// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/un.h>

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

/// The longest socket path: sockaddr_un holds 108 bytes on Linux and 104 on
/// FreeBSD, the NUL included.
inline constexpr std::size_t MaxSocketPathBytes = sizeof(sockaddr_un::sun_path) - 1;

/// The longest host name, without its NUL: HOST_NAME_MAX, which FreeBSD does
/// not define (its MAXHOSTNAMELEN counts the NUL).
#ifdef HOST_NAME_MAX
inline constexpr std::size_t MaxHostNameBytes = HOST_NAME_MAX;
#else
inline constexpr std::size_t MaxHostNameBytes = MAXHOSTNAMELEN - 1;
#endif

/// The effective user and the process at the other end of a connected Unix
/// socket, as the kernel recorded them at connect(): SO_PEERCRED on Linux,
/// LOCAL_PEERCRED on FreeBSD. False when the kernel does not tell.
struct PeerCredentials
{
    uid_t uid = uid_t(-1);
    pid_t pid = 0;
};
bool peerCredentials(int socket, PeerCredentials& out);

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
/// --class, and one that could not hand its window to the main afterglow:
/// another executable, a main afterglow that it cannot verify, a command too
/// long to hand over, or no answer (main.cpp). Empty as sharedSocketPath()
/// is.
std::string ownSocketPath(std::int64_t pid, const EnvironmentLookup& env = systemEnvironment);

struct SocketLookup
{
    std::string path; ///< empty when there is none: error says why
    /// The socket of $AFTERGLOW_SOCKET. A pane of a window sets it to the
    /// socket of the Afterglow that runs the pane. A pane of a mux session
    /// sets it to the socket of the mux daemon, which serves the `control`
    /// target for the session, and passes a client of the `commands` target
    /// to the window that shows the session.
    bool own = false;
    /// $AFTERGLOW_PANE_ID whenever $AFTERGLOW_SOCKET is set, else 0. The
    /// lookup cannot tell a socket of another Afterglow from the pane's own:
    /// a user who points AFTERGLOW_SOCKET at another one clears
    /// AFTERGLOW_PANE_ID, or gives the client a pane.
    std::int64_t pane = 0;
    /// $AFTERGLOW_SESSION is set with $AFTERGLOW_SOCKET: a pane of a mux
    /// session, whose socket is the mux daemon's.
    bool session = false;
    std::string error;
};

/// The socket of a client: $AFTERGLOW_SOCKET, the Afterglow that runs this
/// pane, else the shared socket of the display. A pane ID without a socket
/// is an error: another afterglow on the display numbers its panes from 1
/// too, and would act on a pane with the same number. So is a path over
/// MaxSocketPathBytes, and an $AFTERGLOW_SOCKET that is not an absolute path:
/// each client would find it from its own working directory.
SocketLookup findSocket(const EnvironmentLookup& env = systemEnvironment);

/// Who listens at the socket of @p lookup, for a message: "the Afterglow of
/// this display", "the mux daemon of this session pane", "the Afterglow that
/// runs this pane" or "the Afterglow at $AFTERGLOW_SOCKET".
std::string socketOwner(const SocketLookup& lookup);
/// Why nobody listens at the socket of @p lookup (ENOENT, ECONNREFUSED), by
/// what is at its path now. In a pane of a mux session whose socket file is
/// gone, it says so: logind removes the runtime directory when the last
/// login of the user ends, and the daemon goes on at its other socket.
std::string noServerReason(const SocketLookup& lookup);

/// The name of the directories of the mux daemon's sockets (muxSockets()).
inline constexpr std::string_view MuxDirectoryName = "aglow-mux";
/// Whether @p path names a file in such a directory: a socket of the mux
/// daemon, or of an ssh session in a session pane (remote-ID-TOKEN.sock).
bool inMuxDirectory(std::string_view path);

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
///   $XDG_RUNTIME_DIR/aglow-mux/socket
///   $XDG_STATE_HOME/aglow-mux/<muxStateSocketName()> (by default under ~/.local/state)
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
