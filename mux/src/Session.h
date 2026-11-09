// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxPane.h"
#include "MuxProtocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AfterglowMux {

/// One set of windows, tabs and panes with running processes. The daemon
/// owns the layout. Until a client can attach, a session has one window
/// with one pane.
class Session
{
public:
    /// @p fileLimit is the soft RLIMIT_NOFILE that the programs get: the
    /// daemon's own before it raised it, or -1 for the daemon's.
    Session(std::string name, SessionOptions options, long fileLimit);

    const std::string& name() const { return m_name; }

    /// Starts a pane with the session's launch, its environment and umask,
    /// and the variables that name the session, the pane and the daemon
    /// socket. False when its program cannot start: @p error says why.
    bool addPane(std::uint32_t id, const std::string& daemonSocket, std::function<void()> exited, std::string* error);
    /// Removes the panes whose programs exited. True when none is left: the
    /// last program of a session ended, and so does the session.
    /// @p closed gets the id and wait status of each pane removed.
    bool reap(std::vector<std::pair<std::uint32_t, int>>* closed = nullptr);
    const std::vector<std::unique_ptr<MuxPane>>& panes() const { return m_panes; }
    MuxPane* pane(std::uint32_t id) const
    {
        for (const auto& p : m_panes) {
            if (p->id() == id) return p.get();
        }
        return nullptr;
    }
    /// Ends every program of the session.
    void kill();

    /// What listSessions says of the session to @p caller, the process of
    /// the client that asks (0 when unknown): see MuxPane::foregroundJob.
    SessionInfo info(pid_t caller) const;

private:
    std::string m_name;
    SessionOptions m_options;
    long m_fileLimit;
    std::vector<std::unique_ptr<MuxPane>> m_panes;
};

} // namespace AfterglowMux
