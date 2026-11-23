// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Socket.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

// How a client reaches the mux daemon, with the same lookup for each:
// `afterglow mux`; a window that attaches to a session, and its later calls
// to the daemon (MuxClient); the projects and the new sessions of the
// application, which reach and start the daemon from a worker thread of the
// GUI process (MuxSessions); and `aglow-mux proxy` on another host.

namespace AfterglowMux {

/// How long a client waits to connect to a socket, and for the answer to a call.
inline constexpr int ConnectTimeoutMs = 1000;
inline constexpr int CallTimeoutMs = 5000;
/// The wait for a call that starts a program, newSession and newPane: the
/// daemon waits up to the PTY's exec wait (2 s) for the program to start,
/// and a client may call while it waits for the programs of other calls.
inline constexpr int SpawnCallTimeoutMs = 10000;

/// The command line of the proxy on another host: `aglow-mux proxy
/// [--start]` (mux/src/main.cpp reads it, reach() writes it).
inline constexpr const char* ProxyArgument = "proxy";
inline constexpr const char* ProxyStartOption = "--start";

/// An environment, as NAME=VALUE entries.
using Environment = std::vector<std::string>;
/// This process's environment.
Environment currentEnvironment();
/// The path of @p name on the PATH of @p environment, as execvp finds it,
/// or empty. For a spawn off the thread that changes the environment:
/// posix_spawnp reads PATH from the environment of this process.
std::string findProgram(const std::string& name, const Environment& environment);

/// The exit status of `aglow-mux proxy` without --start when no daemon
/// runs: no daemon, so no sessions, and none starts.
inline constexpr int ProxyNoDaemon = 3;

/// The process of a proxy command (ssh that runs `aglow-mux proxy` on
/// another host), which carries a connection to the daemon there on its
/// standard input and output. It ends with the connection: its owner closes
/// the connection first, and then this reaps it. SIGCHLD must have its
/// default action: else the exit status is lost, and with it the 127 of a
/// host without aglow-mux and ProxyNoDaemon.
class ProxyProcess
{
public:
    ProxyProcess(pid_t pid, int errorFd)
        : m_pid(pid)
        , m_errorFd(errorFd)
    {
    }
    /// Waits up to 1 s for its end, then ends it: up to 2 s more. A thread
    /// that must not wait gives the last reference to endInBackground().
    ~ProxyProcess();
    ProxyProcess(const ProxyProcess&) = delete;
    ProxyProcess& operator=(const ProxyProcess&) = delete;
    /// Waits up to @p timeoutMs for its end: its exit status, -1 for a
    /// signal or no end. Reads what it wrote on its standard error meanwhile.
    int wait(int timeoutMs);
    /// What it wrote on its standard error, at most its first 4 KiB: each
    /// line safe to print (printable()), without the newlines at its end.
    std::string errorOutput() const;
    /// The read end of its standard error, which does not block, or -1. A
    /// caller that keeps the connection open (a window's stream) watches it
    /// and calls drainError() when it is readable: a command that writes
    /// much would else block on a full pipe and stop the connection.
    int errorFd() const
    {
        return m_errorFd;
    }
    /// Reads what waits on errorFd(). False once the pipe ended: nothing
    /// more comes, and errorFd() stays open until this goes.
    bool drainError();

private:
    pid_t m_pid = -1;
    int m_errorFd = -1;
    bool m_errorEnded = false;
    int m_status = -2; ///< -2: not reaped yet
    std::string m_error;
};

/// Drops @p proxy on a thread of its own, for a thread that must not wait
/// for its end (a window's): the last reference waits as ~ProxyProcess
/// does. Its connection closed before.
void endInBackground(std::shared_ptr<ProxyProcess> proxy);

struct Reached
{
    Reached() = default;
    Reached(std::shared_ptr<ProxyProcess> proxy, std::unique_ptr<AfterglowProto::IpcConnection> connection)
        : proxy(std::move(proxy))
        , connection(std::move(connection))
    {
    }
    Reached(Reached&&) noexcept = default;
    /// The connection closes first, and then the proxy that carried it
    /// goes: the close ends the proxy, so `reached = {}` waits only for that
    /// end, not for the timeouts of ~ProxyProcess.
    Reached& operator=(Reached&& other) noexcept
    {
        if (this != &other) {
            connection = std::move(other.connection);
            proxy = std::move(other.proxy);
        }
        return *this;
    }

    /// The proxy command that carries the connection, for a daemon on
    /// another host. First: it goes after the connection, which ends it.
    std::shared_ptr<ProxyProcess> proxy;
    std::unique_ptr<AfterglowProto::IpcConnection> connection;
};

/// How a client reaches a daemon: its sockets on this computer, or a command
/// that runs `aglow-mux proxy` on another host and carries the
/// connection on its standard input and output (ssh over the shared
/// connection of `afterglow ssh`).
struct Route
{
    AfterglowProto::MuxSockets sockets;
    /// The command without "proxy": ssh, its options, the destination and
    /// the program of Mux.remoteCommand there (by default aglow-mux, which
    /// the PATH of the login there finds). Empty for this computer.
    std::vector<std::string> proxy;
    /// The destination, for messages.
    std::string host;
};

/// The first candidate whose daemon answers the version call, in the order
/// of muxSockets(). Without one, no connection: @p error is empty when no
/// daemon listens on any, and says why when one cannot be used (another
/// protocol, no answer, a socket of another user). @p timeoutMs, when not
/// -1, bounds the whole lookup: a daemon that does not answer by then
/// counts as none.
Reached connectAny(const AfterglowProto::MuxSockets& sockets, std::string* error, int timeoutMs = -1);

/// The daemon of @p route. On this computer, connectAny(). Elsewhere, the
/// proxy command, which with @p start starts the daemon there when none
/// answers. Its first answer comes within @p proxyTimeoutMs: the sign-in
/// wait (Mux.signInTimeout) for a command that ssh may first ask for a
/// password or a passphrase, else CallTimeoutMs. Without a connection,
/// @p error is empty when no daemon runs, and says why otherwise, safe to
/// print: the command failed, the host has no aglow-mux, the login there
/// writes on standard output, or the daemon speaks another protocol.
/// SIGCHLD must have its default action (ProxyProcess). The proxy command
/// runs in @p environment, and is found on its PATH: a thread other than the
/// one that changes the environment of this process passes a copy.
Reached reach(const Route& route, bool start, int proxyTimeoutMs, const Environment& environment, std::string* error);
/// reach() in the environment of this process.
Reached reach(const Route& route, bool start, int proxyTimeoutMs, std::string* error);

} // namespace AfterglowMux
