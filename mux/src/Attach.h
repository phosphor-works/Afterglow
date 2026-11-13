// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxPane.h"
#include "MuxStream.h"

#include <AfterglowVt/StateSync.h>
#include <PhosphorChannel/Stream.h>

#include <chrono>
#include <cstdint>
#include <deque>
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

/// The window that a session shows in. The attach call makes it, and its own
/// connection carries the window's stream from then on: a phosphor-channel
/// stream with the session's layout and state to the window, and the
/// window's input, its layout and the panes that it closes back
/// (MuxStream.h). So the window reaches it as it reached the daemon, also
/// through a proxy on another host (`aglow-mux proxy`).
///
/// A pane's frame that the channel refuses waits, and no newer frame is made
/// until it went: a frame covers every change since the one before, so the
/// output that comes meanwhile is in the next one. A slow window sees fewer
/// frames, never a growing queue. Input waits too when the program's PTY has
/// no room for it, or when a pane that waits for a decode already holds its
/// bound of input. The window's input waits behind it.
///
/// The daemon never blocks on a window. A pane whose program exited keeps
/// its terminal here until its last frames and calls went; a takeover, a
/// detach and the end of the session finish the attachment. It gives the
/// host calls that wait back to their panes (returnHostCalls()), and sends
/// what else waits and its last event, up to FinishWaitMs.
class Attachment
{
public:
    /// How long a finished attachment may take to send what waits, in
    /// milliseconds.
    static constexpr int FinishWaitMs = 2000;

    /// An attachment that shows @p session once it has its connection
    /// (adopt()).
    explicit Attachment(std::string session);
    ~Attachment();
    Attachment(const Attachment&) = delete;
    Attachment& operator=(const Attachment&) = delete;

    /// Takes @p fd, the connection whose attach call made it, after the
    /// daemon answered the call: @p unsent is the end of that answer, which
    /// goes first. The window writes first, as its stream opens its
    /// channels: until then nothing more goes, so that the reader of the
    /// window's call takes no byte of the stream.
    void adopt(int fd, std::string unsent);

    const std::string& session() const { return m_session; }
    /// The window's stream started: it wrote its first bytes.
    bool connected() const { return m_stream != nullptr; }
    /// finish() ran: the session no longer shows here.
    bool finishing() const { return m_finish.has_value(); }

    /// The stream failed: the attachment is over.
    bool failed() const { return m_failed; }
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
    /// The time left for the window to start its stream, or for a finished
    /// attachment to send what waits; -1 when neither waits.
    int timeoutMs() const;
    /// The window asked to end the session (Attach::InputType::Kill). Once.
    bool takeKill() { return std::exchange(m_kill, false); }

    /// Takes the pane whose program exited: its last frames go, then its
    /// channel closes and PaneClosed follows.
    void paneClosed(std::unique_ptr<MuxPane> pane);
    /// Pane @p id left the session, at the window's ClosePane or for another
    /// session (Fn::BreakOut): the window no longer shows it, so its channel
    /// closes at once and PaneClosed follows, with no status. The host calls
    /// of a pane that goes on in another session, @p moved, get the answers
    /// of a pane that no window shows: this window does not get them.
    void paneRemoved(std::uint32_t id, MuxPane* moved = nullptr);
    /// Ends the attachment: input is no longer read, and once the panes that
    /// closed sent their last frames, @p type is the last event. A window
    /// that did not connect yet gets it when it does. The host calls that
    /// did not go yet go back to the panes of @p session (returnHostCalls()).
    void finish(Attach::EventType type, Session* session = nullptr);
    /// Drops the host calls that wait for the channel. Those of the live panes
    /// of @p session get the answers of a pane that no window shows: the
    /// window never gets them. A call whose first parts went goes on unless
    /// @p windowGone: then the stream sends nothing more.
    void returnHostCalls(Session* session, bool windowGone = false);
    /// The bytes of the events that wait in the daemon's queue for the
    /// window: Mux.eventQueue, with Host::CallCost for each. Host calls wait
    /// in each pane's queue past it (MuxPane::setWindow). A pump takes at
    /// least one call, so the queue can pass it by one call.
    void setEventQueue(std::size_t bytes) { m_eventQueue = bytes; }
    std::size_t eventQueue() const { return m_eventQueue; }
    /// The most history rows that the window keeps for each pane: the
    /// frames carry no older ones (StateWriter::setReplicaHistory).
    void setReplicaHistory(std::size_t rows) { m_replicaHistory = rows; }
    /// A window shows the session's panes now: connected, and not finishing.
    bool showing() const { return m_stream && !m_finish && !m_failed; }
    /// A number that no other attachment of the daemon has: which window a
    /// pane's host calls wait for (MuxPane::setWindow).
    std::uint64_t serial() const { return m_serial; }
    /// A call of the control target for the window to answer (an
    /// Attach::EventType::Control event): @p json is its {"id", "fn", "args"}.
    void sendControl(std::string json);
    /// The answers of the window to those calls, as their JSON, oldest first.
    std::vector<std::string> takeControlReplies() { return std::exchange(m_controlReplies, {}); }
    /// A part of a command connection that the window serves (an
    /// Attach::EventType::Commands event).
    void sendRelay(const Attach::RelayFrame& frame);
    /// The events that wait for the window are under eventQueue(): the
    /// daemon reads the next line of a command client. A client that sends
    /// faster than the window takes waits in its socket.
    bool relayRoom() const { return m_waitingBytes < m_eventQueue; }
    /// The parts of command connections that the window sent, oldest first:
    /// Bytes and End only.
    std::vector<Attach::RelayFrame> takeRelayFrames() { return std::exchange(m_relayFrames, {}); }

private:
    struct PaneLink
    {
        std::uint32_t channel = 0;
        PhosphorChannel::StreamWriter* writer = nullptr;
        AfterglowVt::StateWriter state;
        std::string waiting; ///< a frame that the channel refused
        bool started = false; ///< the first frame went
        bool deferred = false; ///< the last frame waits for a decode: the host calls after it wait too
        std::uint64_t frames = 0; ///< the frames written on the channel, which each host call names
        std::unique_ptr<MuxPane> closed; ///< the pane once its program exited
    };

    /// The window wrote its first bytes: its stream starts here too.
    void startStream();
    void readInput(Session& session);
    /// Sends the input that waits for room in a PTY. False while it waits.
    bool sendHeldInput(Session& session);
    PaneLink* link(MuxPane& pane);
    /// Sends the next frame of @p pane. True when nothing of it waits: no
    /// refused frame, no change and no history.
    bool sendFrame(PaneLink& link, MuxPane& pane);
    /// Sends the host calls of @p pane that fit the queue, in parts when a
    /// call is larger than one frame (Attach::HostPartBytes), each with the
    /// frames that @p link wrote before it.
    void sendHostCalls(const PaneLink& link, MuxPane& pane);
    /// @p call: the host call that the event holds or holds a part of.
    void sendEvent(const Attach::Event& event, std::shared_ptr<const std::string> call = {});
    void flushEvents();
    bool finished() const;

    int m_fd = -1; ///< the connection, until the stream takes it
    std::string m_unsent; ///< the end of the answer to the attach call
    std::string m_session;
    std::chrono::steady_clock::time_point m_deadline; ///< for the window's stream, then to finish
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_events = nullptr;
    struct WaitingEvent
    {
        std::string encoded;
        std::uint32_t pane = 0;
        /// A host call, or a part of one: the call whole, which goes back to
        /// its pane when the window does not get it.
        std::shared_ptr<const std::string> call;
        bool last = true; ///< not a part that more parts follow
    };
    /// The call whose parts go, after its first went: its other parts go too.
    std::shared_ptr<const std::string> m_partSent;
    /// m_partSent when returnHostCalls() answered it: once.
    std::shared_ptr<const std::string> m_answeredPart;
    std::deque<WaitingEvent> m_waitingEvents; ///< events that the channel did not take yet
    std::size_t m_waitingBytes = 0; ///< with Host::CallCost for each
    std::size_t m_eventQueue = std::size_t(DefaultEventQueue);
    std::size_t m_replicaHistory = ~std::size_t(0);
    std::uint64_t m_serial = 0;
    std::map<std::uint32_t, PaneLink> m_panes; ///< by pane id
    std::optional<Attach::Input> m_heldInput; ///< input that a pane refused (MuxPane::Input::Full)
    bool m_inputDue = false; ///< held input went in pump(): the next handle() reads what follows it
    std::optional<Attach::EventType> m_finish;
    bool m_finishSent = false;
    bool m_layoutSent = false; ///< the first event, before any PaneOpened
    std::vector<std::string> m_controlReplies; ///< Attach::InputType::ControlReply, for the daemon
    std::vector<Attach::RelayFrame> m_relayFrames; ///< from CommandsChannel, for the daemon
    bool m_outputEnded = false; ///< all went, and the stream's output ended: the window closes

    bool m_kill = false;
    bool m_failed = false;
};

} // namespace AfterglowMux
