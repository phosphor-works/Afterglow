// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Session.h"

#include <AfterglowProto/IpcServer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AfterglowMux {

/// The headless process that owns the sessions. One thread runs the control
/// socket and the lifetimes of sessions and panes. Each pane's PTY has its
/// reader thread, which wakes this one through an eventfd when its program
/// exits.
class Daemon
{
public:
    Daemon();
    ~Daemon();
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    /// Listens on @p path, after it creates @p directory (mode 0700) when it
    /// is not empty. False when another daemon listens there: @p error says why.
    bool listen(const std::string& path, const std::string& directory, std::string* error);
    /// Serves until no session is left, or until SIGTERM, SIGINT or SIGHUP.
    /// Returns the exit status of the process.
    int run();

private:
    void accept();
    void read(AfterglowProto::IpcServerConnection& connection);
    std::string answer(const AfterglowProto::IpcRequest& request);
    std::string newSession(const AfterglowProto::IpcRequest& request);
    std::string killSession(const AfterglowProto::IpcRequest& request);
    std::string listSessions(const AfterglowProto::IpcRequest& request);
    Session* find(const std::string& name);
    std::string freeName() const;
    void reap();
    /// No session and no connection: nothing can need the daemon now.
    bool idle() const;

    AfterglowProto::IpcListener m_listener;
    int m_wakeFd = -1;
    int m_signalFd = -1;
    std::vector<std::unique_ptr<AfterglowProto::IpcServerConnection>> m_connections;
    std::vector<std::unique_ptr<Session>> m_sessions;
    std::uint32_t m_nextPane = 1;
    /// A session was created. Before that, the daemon waits for one for
    /// StartWaitMs; a connection alone does not end the wait (a client may
    /// probe the socket before it calls).
    bool m_hadSession = false;
};

} // namespace AfterglowMux
