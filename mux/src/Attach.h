// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxPane.h"
#include "MuxStream.h"

#include <AfterglowVt/StateSync.h>
#include <PhosphorChannel/Stream.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
/// frames, never a growing queue. Input that the program's PTY has no room
/// for waits too, and the window's input waits behind it.
///
/// The daemon never blocks on a window. A pane whose program exited keeps
/// its terminal here until its last frames went; a takeover, a detach and
/// the end of the session finish the attachment, which then sends what
/// waits and its last event, up to FinishWaitMs.
class Attachment
{
public:
    /// How long a finished attachment may take to send what waits, in
    /// milliseconds.
    static constexpr int FinishWaitMs = 2000;

    /// Listens on a new socket in @p directory for @p caller, to show
    /// @p session. Nullptr with @p error when the socket cannot be made.
    static std::unique_ptr<Attachment> listen(const std::string& directory, std::string session, pid_t caller,
                                              std::string* error);
    ~Attachment();
    Attachment(const Attachment&) = delete;
    Attachment& operator=(const Attachment&) = delete;

    const std::string& session() const { return m_session; }
    const std::string& path() const { return m_path; }
    /// The window connected to the socket.
    bool connected() const { return m_stream != nullptr; }
    /// finish() ran: the session no longer shows here.
    bool finishing() const { return m_finish.has_value(); }

    /// The descriptor to poll and its events.
    pollfd pollFd() const;
    /// Handles what the poll saw on pollFd(), and sends what changed in
    /// @p session, nullptr once finishing(). False when the attachment is
    /// over: the window left or failed, it never came, or a finished one sent
    /// everything or ran out of time.
    bool handle(short revents, Session* session);
    /// Sends what changed in @p session, nullptr once finishing(), and what
    /// waits.
    void pump(Session* session);
    /// A frame can go now: the daemon polls without waiting.
    bool busy() const;
    /// The time left for the window to connect, or for a finished attachment
    /// to send what waits; -1 when neither waits.
    int timeoutMs() const;
    /// The window asked to end the session (Attach::InputType::Kill). Once.
    bool takeKill() { return std::exchange(m_kill, false); }

    /// Takes the pane whose program exited: its last frames go, then its
    /// channel closes and PaneClosed follows.
    void paneClosed(std::unique_ptr<MuxPane> pane);
    /// Ends the attachment: input is no longer read, and once the panes that
    /// closed sent their last frames, @p type is the last event. A window
    /// that did not connect yet gets it when it does.
    void finish(Attach::EventType type);

private:
    struct PaneLink
    {
        std::uint32_t channel = 0;
        PhosphorChannel::StreamWriter* writer = nullptr;
        AfterglowVt::StateWriter state;
        std::string waiting; ///< a frame that the channel refused
        bool started = false; ///< the first frame went
        std::unique_ptr<MuxPane> closed; ///< the pane once its program exited
    };

    Attachment() = default;
    void accept();
    void readInput(Session& session);
    /// Sends the input that waits for room in a PTY. False while it waits.
    bool sendHeldInput(Session& session);
    PaneLink* link(MuxPane& pane);
    /// Sends the next frame of @p pane. True when nothing of it waits: no
    /// refused frame, no change and no history.
    bool sendFrame(PaneLink& link, MuxPane& pane);
    void sendEvent(const Attach::Event& event);
    void flushEvents();
    bool finished() const;

    int m_listenFd = -1;
    std::string m_session;
    std::string m_path;
    pid_t m_caller = 0;
    std::chrono::steady_clock::time_point m_deadline; ///< to connect, then to finish
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_events = nullptr;
    std::vector<std::string> m_waitingEvents; ///< events before the window granted the channel a window
    std::map<std::uint32_t, PaneLink> m_panes; ///< by pane id
    std::optional<Attach::Input> m_heldInput; ///< input that a full PTY refused
    std::optional<Attach::EventType> m_finish;
    bool m_finishSent = false;
    bool m_outputEnded = false; ///< all went, and the stream's output ended: the window closes

    bool m_kill = false;
    bool m_failed = false;
};

} // namespace AfterglowMux
