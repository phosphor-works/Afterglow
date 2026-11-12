// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxLayout.h"
#include "MuxPane.h"
#include "MuxProtocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AfterglowMux {

/// The panes of one session, their running programs, and their tabs and
/// splits (Layout). The window that shows the session changes the layout and
/// sends it; the daemon keeps it for the next window, and takes out the panes
/// whose programs end.
class Session
{
public:
    /// @p fileLimit is the soft RLIMIT_NOFILE that the programs get: the
    /// daemon's own before it raised it, or -1 for the daemon's. @p images
    /// decodes the images of the panes, and outlives the session.
    Session(std::string name, SessionOptions options, long fileLimit, ImageDecoder& images);

    const std::string& name() const { return m_name; }
    const SessionOptions& options() const { return m_options; }

    /// Starts a pane with @p launch over the session's environment and
    /// umask, at @p size, and the variables that name the session, the pane
    /// and the daemon socket: a tab of its own after the others. False when
    /// its program cannot start: @p error says why. @p wake is the pane's
    /// (MuxPane), safe to call from any thread.
    bool addPane(std::uint32_t id, const Launch& launch, const PaneSize& size, const std::string& daemonSocket,
                 std::function<void()> wake, std::string* error);
    /// The launch and the size of the session's first pane.
    const Launch& firstLaunch() const { return m_options.launch; }
    const PaneSize& firstSize() const { return m_options.size; }
    /// Takes out the panes whose programs exited, also from the layout: the
    /// window that shows the session still sends their last frames. Without
    /// a pane left, the last program of the session ended, and so does the
    /// session.
    std::vector<std::unique_ptr<MuxPane>> reap();
    /// Ends the program of pane @p id: the user closed the pane in the window,
    /// which stays. The pane leaves the session and the layout at once. False
    /// when the session has no such pane.
    bool closePane(std::uint32_t id);
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
    /// Takes pane @p id out of the session and its layout, with its program
    /// running: for another session (adopt()). Nullptr when there is none.
    std::unique_ptr<MuxPane> take(std::uint32_t id);
    /// A pane that another session gave up (take()), as a tab of its own.
    void adopt(std::unique_ptr<MuxPane> pane);

    const Layout& layout() const { return m_layout; }
    /// The layout that the window sent, with the panes of the session only:
    /// a pane that exited since the window sent it goes, and a pane that it
    /// does not name yet is a tab of its own (Layout::reconcile).
    void setLayout(Layout layout);

    /// What listSessions says of the session to @p caller, the process of
    /// the client that asks (0 when unknown): see MuxPane::foregroundJob.
    SessionInfo info(pid_t caller) const;

private:
    std::vector<std::uint32_t> paneIds() const;

    std::string m_name;
    SessionOptions m_options;
    long m_fileLimit;
    ImageDecoder& m_images;
    std::vector<std::unique_ptr<MuxPane>> m_panes;
    Layout m_layout;
};

} // namespace AfterglowMux
