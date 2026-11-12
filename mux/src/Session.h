// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxPane.h"
#include "MuxProtocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AfterglowMux {

/// The panes of one session and their running programs. A session has one
/// pane until the daemon keeps a layout of windows, tabs and splits.
class Session
{
public:
    /// @p fileLimit is the soft RLIMIT_NOFILE that the programs get: the
    /// daemon's own before it raised it, or -1 for the daemon's. @p images
    /// decodes the images of the panes, and outlives the session.
    Session(std::string name, SessionOptions options, long fileLimit, ImageDecoder& images);

    const std::string& name() const { return m_name; }

    /// Starts a pane with the session's launch, its environment and umask,
    /// and the variables that name the session, the pane and the daemon
    /// socket. False when its program cannot start: @p error says why.
    /// @p wake is the pane's (MuxPane), safe to call from any thread.
    bool addPane(std::uint32_t id, const std::string& daemonSocket, std::function<void()> wake, std::string* error);
    /// Takes out the panes whose programs exited: the window that shows the
    /// session still sends their last frames. Afterwards no pane may be
    /// left: then the last program of the session ended, and so does the
    /// session.
    std::vector<std::unique_ptr<MuxPane>> reap();
    bool empty() const { return m_panes.empty(); }
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
    ImageDecoder& m_images;
    std::string m_temporaryDirectory; ///< the programs' TMPDIR (Terminal::setTemporaryDirectory)
    std::vector<std::unique_ptr<MuxPane>> m_panes;
};

} // namespace AfterglowMux
