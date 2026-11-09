// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxStream.h"

#include <AfterglowVt/StateSync.h>
#include <PhosphorChannel/Stream.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/types.h>

namespace AfterglowMux {

class Session;

/// The window that a session shows in. The attach call makes it with a
/// socket of its own, where only the process that called may connect; then
/// a phosphor-channel stream on that connection carries the session's state
/// to the window and the window's input back (MuxStream.h).
///
/// A pane's frame that the channel refuses waits, and no newer frame is made
/// until it went: a frame covers every change since the one before, so the
/// output that comes meanwhile is in the next one. A slow window sees fewer
/// frames, never a growing queue.
class Attachment
{
public:
    /// Listens on a new socket in @p directory for @p caller. Nullptr with
    /// @p error when the socket cannot be made.
    static std::unique_ptr<Attachment> listen(const std::string& directory, pid_t caller, std::string* error);
    ~Attachment();
    Attachment(const Attachment&) = delete;
    Attachment& operator=(const Attachment&) = delete;

    const std::string& path() const { return m_path; }
    bool connected() const { return m_stream != nullptr; }

    /// The descriptor to poll and its events.
    pollfd pollFd() const;
    /// Handles what the poll saw on pollFd(), and sends what changed in the
    /// session. False when the window left, failed or never came.
    bool handle(short revents, Session& session);
    /// Sends what changed in the session.
    void pump(Session& session);
    /// More history waits for a frame: the daemon polls without waiting.
    bool busy() const;
    /// The time left for the window to connect, or -1 once it did.
    int connectTimeoutMs() const;

    /// Sends an event and what is queued, and closes: for the end of the
    /// session and a takeover.
    void finish(Attach::EventType type, Session* session);
    void paneClosed(std::uint32_t pane, int status);

private:
    struct PaneLink
    {
        PhosphorChannel::StreamWriter* writer = nullptr;
        AfterglowVt::StateWriter state;
        std::string waiting; ///< a frame that the channel refused
        bool started = false; ///< the first frame went
    };

    Attachment() = default;
    bool accept();
    void readInput(Session& session);
    void sendEvent(const Attach::Event& event);
    void flushEvents();

    int m_listenFd = -1;
    std::string m_path;
    pid_t m_caller = 0;
    std::chrono::steady_clock::time_point m_deadline;
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_events = nullptr;
    std::vector<std::string> m_waitingEvents; ///< events before the window granted the channel a window
    std::map<std::uint32_t, PaneLink> m_panes;
    bool m_failed = false;
};

} // namespace AfterglowMux
