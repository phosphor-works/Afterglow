// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxLayout.h"
#include "MuxPane.h"
#include "MuxStream.h"

#include <AfterglowVt/StateSync.h>
#include <PhosphorChannel/Stream.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/types.h>

namespace AfterglowMux {

class Session;

/// The window that a session shows in. The attach call makes it with a
/// socket of its own, where only the process that called may connect; then
/// a phosphor-channel stream on that connection carries the session's layout
/// and state to the window, and the window's input, its layout and the panes
/// that it closes back (MuxStream.h).
///
/// A pane's frame that the channel refuses waits, and no newer frame is made
/// until it went: a frame covers every change since the one before, so the
/// output that comes meanwhile is in the next one. A slow window sees fewer
/// frames, never a growing queue. Input waits too when the program's PTY has
/// no room for it, or when a pane that waits for a decode already holds its
/// bound of input: in the attachment, for that pane, with the pane's input
/// that follows it. The input of the other panes goes on. Only once the
/// input that waits passes MaxHeldInput does the daemon stop reading the
/// window's input channel, which then fills.
///
/// What a window that does not read makes the daemon hold, for each pane:
/// the frames that the pane's channel takes (the stream's window, 4 MiB), one
/// frame that it refused, and the host calls that wait in the pane
/// (Mux.eventQueue). Then the events that wait (Mux.eventQueue and one call)
/// and the input above. A session has at most MaxSessionPanes panes.
///
/// The daemon never blocks on a window. A pane whose program exited keeps
/// its terminal here until its last frames and calls went; a takeover, a
/// detach and the end of the session finish the attachment. It gives the
/// host calls that wait back to their panes (returnHostCalls()), and sends
/// what else waits and its last event, up to FinishWaitMs. Meanwhile the
/// window may still send what it sent before it heard (takeLate()).
class Attachment
{
public:
    /// How long a finished attachment may take to send what waits, in
    /// milliseconds.
    static constexpr int FinishWaitMs = 2000;
    /// The input that the daemon holds for panes whose PTYs have no room,
    /// in bytes, with PhosphorChannel::Stream::FrameCost for each message:
    /// past it, the daemon reads no more of the window's input.
    static constexpr std::size_t MaxHeldInput = std::size_t(4) << 20;

    /// Listens on a new socket in @p directory for @p caller, to show
    /// @p session. Nullptr with @p error when the socket cannot be made.
    static std::unique_ptr<Attachment> listen(const std::string& directory, std::string session, pid_t caller,
                                              std::string* error);
    ~Attachment();
    Attachment(const Attachment&) = delete;
    Attachment& operator=(const Attachment&) = delete;

    const std::string& session() const { return m_session; }
    const std::string& path() const { return m_path; }
    /// The process that made the attach call: the only one that may change
    /// the panes of the session while this window shows it.
    pid_t caller() const { return m_caller; }
    /// Finds a pane of any session of the daemon by its ID: the answers of
    /// the window to the calls of a pane that went to another session
    /// before the window heard of it still reach the pane's program.
    void setPaneFinder(std::function<MuxPane*(std::uint32_t)> find) { m_findPane = std::move(find); }
    /// The window connected to the socket.
    bool connected() const { return m_stream != nullptr; }
    /// finish() ran: the session no longer shows here.
    bool finishing() const { return m_finish.has_value(); }

    /// The stream failed: the attachment is over.
    bool failed() const { return m_failed; }
    /// The descriptor to poll and its events.
    pollfd pollFd() const;
    /// Handles what the poll saw on pollFd(), and sends what changed in
    /// @p session, the session of its name, or nullptr when there is none.
    /// Once finishing(), it sends nothing of the session, and reads what the
    /// window sent before it heard (takeLate()). False when the attachment is
    /// over: the window left or failed, it never came, or a finished one sent
    /// everything or ran out of time.
    bool handle(short revents, Session* session);
    /// Reads what the window sent so far, before a takeover or a detach
    /// finishes the attachment: its last layout and the panes that it
    /// closed count, and the new window gets them.
    void catchUp(Session& session);
    /// What the window of a finished attachment sent before it heard, which
    /// the daemon applies to the session of its name (Daemon::applyLate):
    /// the panes that it closed, and its last layout. Its input for the
    /// programs went to the panes that remain: the answers to the calls that
    /// it got, and what the user typed. A Kill and a resize do not count.
    struct Late
    {
        std::vector<std::uint32_t> closedPanes;
        std::optional<Layout> layout;
    };
    Late takeLate() { return std::exchange(m_late, {}); }
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
    /// Pane @p id left the session, at the window's ClosePane or for another
    /// session (Fn::BreakOut): the window no longer shows it, so its channel
    /// closes at once and PaneClosed follows, with no status. The host calls
    /// of a pane that goes on in another session, @p moved, get the answers
    /// of a pane that no window shows: this window does not get them. The
    /// bytes that it holds for @p moved, and those that the window sends it
    /// before it hears, still go to its program (setPaneFinder()).
    void paneRemoved(std::uint32_t id, MuxPane* moved = nullptr) { panesRemoved({{id, moved}}); }
    /// paneRemoved() for each pane, with one pass over what waits.
    void panesRemoved(const std::vector<std::pair<std::uint32_t, MuxPane*>>& panes);
    /// Ends the attachment: its input for the session no longer counts but
    /// as takeLate() says, and once the panes that closed sent their last
    /// frames, @p type is the last event. A window that did not connect yet
    /// gets it when it does. The host calls that did not go yet go back to
    /// the panes of @p session (returnHostCalls()).
    void finish(Attach::EventType type, Session* session = nullptr);
    /// Drops the host calls that wait for the channel. Those of the live panes
    /// of @p session get the answers of a pane that no window shows: the
    /// window never gets them. A call whose first parts went goes on unless
    /// @p windowGone: then the stream sends nothing more.
    void returnHostCalls(Session* session, bool windowGone = false);
    /// The bytes of the events that wait in the daemon's queue for the
    /// window: Mux.eventQueue, with Host::CallCost for each. Host calls wait
    /// in each pane's queue past it (MuxPane::setWindow). While the queue is
    /// below it, each pane in turn takes its share of what is left, and at
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

    Attachment() = default;
    void accept();
    void readInput(Session& session);
    /// readInput() once finishing(): what counts goes to m_late.
    void readLate(Session& session);
    /// Gives @p message to its pane. False when the pane's PTY has no room
    /// for its bytes and @p windowLeft is not: then it waits.
    bool deliver(Session& session, const Attach::Input& message, bool windowLeft);
    /// Holds @p message behind the input that waits for its pane.
    void hold(Attach::Input message);
    /// Sends the input that waits for room in each PTY, as far as it goes;
    /// with @p windowLeft, it waits for nothing. True when some went.
    bool sendHeldInput(Session& session, bool windowLeft);
    /// The pane's link, made now with the lowest channel that is free.
    /// Nullptr when no channel is free, until the window reads one that
    /// closed to its end, or when the stream failed (m_failed).
    PaneLink* link(MuxPane& pane);
    /// Sends the next frame of @p pane. True when nothing of it waits: no
    /// refused frame, no change and no history.
    bool sendFrame(PaneLink& link, MuxPane& pane);
    /// Sends the host calls of @p pane that fit @p budget bytes of the queue,
    /// and at least one while the queue is below its limit, in parts when a
    /// call is larger than one frame (Attach::HostPartBytes), each with the
    /// frames that @p link wrote before it.
    void sendHostCalls(const PaneLink& link, MuxPane& pane, std::size_t budget);
    /// The last frames, calls and PaneClosed of the panes whose programs
    /// exited.
    void sendClosed();
    /// @p call: the host call that the event holds or holds a part of.
    void sendEvent(const Attach::Event& event, std::shared_ptr<const std::string> call = {});
    void flushEvents();
    bool finished() const;

    int m_listenFd = -1;
    std::string m_session;
    std::string m_path;
    pid_t m_caller = 0;
    std::chrono::steady_clock::time_point m_deadline; ///< to connect, then to finish
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
    /// Panes whose programs exited before they had a channel: they link
    /// once one is free, and then close as the others do.
    std::vector<std::unique_ptr<MuxPane>> m_closedUnlinked;
    /// No channel was free in this pump: the other panes do not ask again.
    bool m_noChannel = false;
    /// The pane whose turn comes first in the next pump.
    std::size_t m_turn = 0;
    /// By pane: the input that its PTY refused (MuxPane::Input::Full), and
    /// the pane's input that came after it, in order.
    std::map<std::uint32_t, std::deque<Attach::Input>> m_heldInput;
    std::size_t m_heldBytes = 0; ///< of m_heldInput, as MaxHeldInput counts
    bool m_inputDue = false; ///< held input went in pump(): the next handle() reads what follows it
    Late m_late;
    std::function<MuxPane*(std::uint32_t)> m_findPane;
    /// The panes that went to another session while this window showed them.
    std::set<std::uint32_t> m_movedPanes;
    std::optional<Attach::EventType> m_finish;
    bool m_finishSent = false;
    bool m_layoutSent = false; ///< the first event, before any PaneOpened
    bool m_outputEnded = false; ///< all went, and the stream's output ended: the window closes

    bool m_kill = false;
    bool m_failed = false;
};

} // namespace AfterglowMux
