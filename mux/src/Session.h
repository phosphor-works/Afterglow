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

/// One set of windows, tabs and panes with running processes. The daemon
/// owns the layout. Until a client can attach, a session has one window
/// with one pane.
class Session
{
public:
    Session(std::string name, SessionOptions options);

    const std::string& name() const { return m_name; }

    /// Starts a pane with the session's launch, its environment and the
    /// variables that name the session, the pane and the daemon socket.
    MuxPane* addPane(std::uint32_t id, const std::string& daemonSocket, std::function<void()> exited, std::string* error);
    /// Removes the panes whose programs exited. True when none is left: the
    /// last program of a session ended, and so does the session.
    bool reap();
    /// Ends every program of the session.
    void kill();

    SessionInfo info() const;

private:
    std::string m_name;
    SessionOptions m_options;
    std::vector<std::unique_ptr<MuxPane>> m_panes;
};

} // namespace AfterglowMux
