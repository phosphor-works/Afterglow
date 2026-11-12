// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Socket.h>

#include <memory>
#include <string>
#include <vector>

#include <sys/types.h>

// How a client reaches the mux daemon: `afterglow mux` and a window that
// attaches to a session use the same lookup.

namespace AfterglowMux {

/// How long a client waits to connect to a socket, and for the answer to a call.
inline constexpr int ConnectTimeoutMs = 1000;
inline constexpr int CallTimeoutMs = 5000;
/// The wait for newSession: the daemon waits up to the PTY's exec wait (2 s)
/// for the program to start, and a client may call while it waits for
/// another session's.
inline constexpr int NewSessionTimeoutMs = 10000;

/// The exit status of `afterglow-mux proxy` without --start when no daemon
/// runs: no daemon, so no sessions, and none starts.
inline constexpr int ProxyNoDaemon = 3;
/// How long the first call through a proxy command waits: ssh may ask for a
/// password or a passphrase first.
inline constexpr int ProxyCallTimeoutMs = 120000;

/// The process of a proxy command (ssh that runs `afterglow-mux proxy` on
/// another host), which carries a connection to the daemon there on its
/// standard input and output. It ends with the connection: its owner closes
/// the connection first, and then this reaps it.
class ProxyProcess
{
public:
    ProxyProcess(pid_t pid, int errorFd)
        : m_pid(pid)
        , m_errorFd(errorFd)
    {
    }
    ~ProxyProcess();
    ProxyProcess(const ProxyProcess&) = delete;
    ProxyProcess& operator=(const ProxyProcess&) = delete;
    /// Waits up to @p timeoutMs for its end: its exit status, -1 for a
    /// signal or no end. Reads what it wrote on its standard error meanwhile.
    int wait(int timeoutMs);
    /// What it wrote on its standard error, without the newline at its end.
    const std::string& errorOutput() const { return m_error; }

private:
    void readError();
    pid_t m_pid = -1;
    int m_errorFd = -1;
    int m_status = -2; ///< -2: not reaped yet
    std::string m_error;
};

struct Reached
{
    std::unique_ptr<AfterglowProto::IpcConnection> connection;
    std::string socket;
    /// The proxy command that carries the connection, for a daemon on
    /// another host. It goes after the connection.
    std::shared_ptr<ProxyProcess> proxy;
};

/// How a client reaches a daemon: its sockets on this computer, or a command
/// that runs `afterglow-mux proxy` on another host and carries the
/// connection on its standard input and output (ssh over the shared
/// connection of `afterglow ssh`).
struct Route
{
    AfterglowProto::MuxSockets sockets;
    /// The command without "proxy": ssh, its options, the destination and
    /// the path of afterglow-mux there. Empty for this computer.
    std::vector<std::string> proxy;
    /// The destination, for messages.
    std::string host;
};

/// The first candidate whose daemon answers the version call, in the order
/// of muxSockets(). Without one, no connection: @p error is empty when no
/// daemon listens on any, and says why when one cannot be used (another
/// protocol, no answer, a socket of another user).
Reached connectAny(const AfterglowProto::MuxSockets& sockets, std::string* error);

/// The daemon of @p route. On this computer, connectAny(). Elsewhere, the
/// proxy command, which with @p start starts the daemon there when none
/// answers. Without a connection, @p error is empty when no daemon runs, and
/// says why otherwise: the command failed, the host has no afterglow-mux, or
/// the daemon speaks another protocol.
Reached reach(const Route& route, bool start, std::string* error);

} // namespace AfterglowMux
