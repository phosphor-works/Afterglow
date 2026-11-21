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
#include <utility>
#include <vector>

namespace AfterglowMux {

/// The panes of one session, their running programs, and their tabs and
/// splits (Layout). The window that shows the session changes the layout and
/// sends it; the daemon keeps it for the next window. The daemon changes it
/// too: a pane that it starts is a tab of its own, and a pane whose program
/// ends, that the window or `afterglow ctl close` closes, or that leaves for
/// another session leaves it. The calls of `afterglow ctl` change it also
/// while no window shows the session (changeLayout()): they place the panes
/// of a split and a new tab, move panes and tabs, and set the focus. Each
/// pane of the session is in the layout once, and no other.
class Session
{
public:
    /// @p fileLimit is the soft RLIMIT_NOFILE that the programs get: the
    /// daemon's own before it raised it, or -1 for the daemon's. @p images
    /// decodes the images of the panes, and outlives the session.
    Session(std::string name, SessionOptions options, long fileLimit, ImageDecoder& images);

    const std::string& name() const
    {
        return m_name;
    }
    /// The daemon's number of the session: a later session of the same name
    /// has another.
    std::uint64_t serial() const
    {
        return m_serial;
    }
    void setSerial(std::uint64_t serial)
    {
        m_serial = serial;
    }
    /// Whether `afterglow ctl watch` watches the session: its panes, and
    /// those that start or come later, keep what their terminals tell for it.
    void setWatched(bool watched);
    /// The watch events of a pane that left the session (take(), reap()),
    /// which `afterglow ctl watch` hears before the pane's end.
    struct LeftEvents
    {
        std::uint32_t pane = 0;
        std::vector<MuxPane::WatchEvent> events;
    };
    /// Those of the panes that left since the last take, oldest first.
    std::vector<LeftEvents> takeLeftEvents()
    {
        return std::exchange(m_left, {});
    }
    /// A pane moved, by a window's layout or by `afterglow ctl`: kept for
    /// `afterglow ctl watch` while the session is watched.
    void noteMoved(std::uint32_t pane);
    std::vector<std::uint32_t> takeMoved()
    {
        return std::exchange(m_moved, {});
    }
    const SessionOptions& options() const
    {
        return m_options;
    }

    /// Starts a pane with @p launch over the session's environment and
    /// umask, at @p size, and the variables that name the session, the pane
    /// and the daemon socket: a tab of its own after the others. False when
    /// its program cannot start: @p error says why. @p wake is the pane's
    /// (MuxPane), safe to call from any thread. @p window and @p windowQueue
    /// name the window that shows the session (MuxPane::setWindow), 0 for
    /// none: the program's first calls to its host already wait for it.
    bool addPane(std::uint32_t id, const Launch& launch, const PaneSize& size, const std::string& daemonSocket,
                 std::uint64_t window, std::size_t windowQueue, std::function<void()> wake, std::string* error);
    /// The launch and the size of the session's first pane.
    const Launch& firstLaunch() const
    {
        return m_options.launch;
    }
    const PaneSize& firstSize() const
    {
        return m_options.size;
    }
    /// Takes out the panes whose programs exited, also from the layout: the
    /// window that shows the session still sends their last frames. Without
    /// a pane left, the last program of the session ended, and so does the
    /// session.
    std::vector<std::unique_ptr<MuxPane>> reap();
    /// Ends the program of pane @p id: the user closed the pane in the window,
    /// or `afterglow ctl close` closed it, also as late input of a window
    /// that no longer shows the session (Daemon::applyLate). The pane leaves
    /// the session and the layout at once. When it was the last pane, the
    /// session is empty(), and the daemon ends it. False when the session
    /// has no such pane.
    bool closePane(std::uint32_t id)
    {
        return take(id) != nullptr;
    }
    bool empty() const
    {
        return m_panes.empty();
    }
    const std::vector<std::unique_ptr<MuxPane>>& panes() const
    {
        return m_panes;
    }
    MuxPane* pane(std::uint32_t id) const
    {
        for (const auto& p : m_panes) {
            if (p->id() == id)
                return p.get();
        }
        return nullptr;
    }
    /// Ends every program of the session.
    void kill();
    /// Takes pane @p id out of the session and its layout, with its program
    /// running: for another session (adopt()). Nullptr when there is none.
    /// Needs no allocation, so a pane that leaves is never lost on the way:
    /// out of memory, only its watch events (takeLeftEvents()) are.
    std::unique_ptr<MuxPane> take(std::uint32_t id);
    /// The panes that another session gave up (take()), with their layout,
    /// which names each of them once and no other pane, and whose values the
    /// caller resolved (keepCommands()): the panes of a session that has none
    /// yet. Allocates nothing.
    void adopt(std::vector<std::unique_ptr<MuxPane>> panes, Layout layout) noexcept;
    /// Room for one more pane, for join(): made before the pane leaves its
    /// session, so that a failure (out of memory) leaves it there.
    void reservePane()
    {
        m_panes.reserve(m_panes.size() + 1);
    }
    /// Takes pane @p pane, which another session gave up (take()), with its
    /// program running: Fn::JoinPane. The caller made room for it
    /// (reservePane()) and puts it in the layout (changeLayout()).
    /// Allocates nothing.
    void join(std::unique_ptr<MuxPane> pane) noexcept;

    const Layout& layout() const
    {
        return m_layout;
    }
    /// For the calls of `afterglow ctl` that change the layout while no
    /// window shows the session, or before the window sends its own: each
    /// call counts in layoutChanges().
    Layout& changeLayout()
    {
        ++m_layoutChanges;
        return m_layout;
    }
    /// The changes of changeLayout() so far: a window that finished before
    /// one (Attachment::Late) did not see it, and its late layout would undo
    /// it.
    std::uint64_t layoutChanges() const
    {
        return m_layoutChanges;
    }
    /// The pane that has the focus in the tab that shows, 0 without a tab.
    std::uint32_t focusedPane() const
    {
        return m_layout.current < m_layout.tabs.size() ? m_layout.tabs[m_layout.current].focused : 0;
    }
    /// The layout that the window sent, with the panes of the session only:
    /// a pane that exited since the window sent it goes, and a pane that it
    /// does not name yet is a tab of its own (Layout::reconcile). A pane
    /// whose command the window does not give keeps the one that the daemon
    /// has: a window of a session of another host runs none, and sends none.
    void setLayout(Layout layout);
    /// Resolves the values of @p layout that keep the command
    /// (PaneValues::keepsCommand) with the commands of @p kept, the layout
    /// that the daemon has for those panes, within MaxLayoutValueBytes: no
    /// value of @p layout keeps a command after it, and none is empty.
    static void keepCommands(Layout& layout, const Layout& kept);
    /// The settings of the window that shows the session: its panes, and
    /// those that start later, take the terminal values and
    /// Mux.keptNotifications, the calls of
    /// `afterglow ctl` and `afterglow ssh` follow the control values, and
    /// the panes that start later get the variables.
    void applySettings(const SessionSettings& settings);
    /// Makes the session an agent session for the rest of its life
    /// (Fn::AdoptSession): it keeps read access, and is never bound.
    void adopt()
    {
        m_options.agent = true;
        m_options.control.allowReadText = true;
        m_adopted = true;
    }
    bool adopted() const
    {
        return m_adopted;
    }

    /// What listSessions says of the session to @p caller, the process of
    /// the client that asks (0 when unknown): see MuxPane::foregroundJob.
    SessionInfo info(pid_t caller) const;

private:
    std::vector<std::uint32_t> paneIds() const;
    /// Keeps the watch events of @p pane, which leaves the session, for
    /// takeLeftEvents(). Out of memory, they are lost.
    void keepLeftEvents(MuxPane& pane) noexcept;

    std::string m_name;
    SessionOptions m_options;
    bool m_adopted = false;
    std::uint64_t m_serial = 0;
    bool m_watched = false;
    std::vector<std::uint32_t> m_moved;
    std::vector<LeftEvents> m_left;
    std::uint64_t m_layoutChanges = 0;
    long m_fileLimit;
    ImageDecoder& m_images;
    std::vector<std::unique_ptr<MuxPane>> m_panes;
    Layout m_layout;
};

} // namespace AfterglowMux
