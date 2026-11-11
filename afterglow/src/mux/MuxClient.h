// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxHost.h"
#include "MuxProtocol.h"
#include "MuxStream.h"

#include <AfterglowProto/Socket.h>
#include <AfterglowPty/Pty.h>

#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class QSocketNotifier;

namespace PhosphorChannel {
class Stream;
class StreamWriter;
} // namespace PhosphorChannel

namespace Afterglow {

class Pane;

/// Why the window no longer shows the session.
enum class MuxLoss : std::uint8_t {
    Ended, ///< the session ended (`afterglow mux kill`, or its last program)
    Detached, ///< `afterglow mux detach`
    TakenOver, ///< another window shows the session now
    Broken, ///< the daemon ended or failed, or sent a frame that does not apply
};

/// A window's link to a session of the mux daemon: the stream that the attach
/// call opens (MuxStream.h). The state of each pane of the session comes in
/// frames, which the Pane applies to its replica terminal. The calls of the
/// session's terminal to its host come as events, and the pane answers them
/// (MuxHost.h). The pane's input and answers go back. It runs on the GUI
/// thread, on socket notifiers.
class MuxClient : public QObject
{
    Q_OBJECT
public:
    /// The most input that waits while the channel is full, in bytes: as much
    /// as a pane's own PTY queues. Input past it is refused, as a PTY refuses it.
    static constexpr std::size_t MaxWaitingInput = AfterglowPty::Pty::MaxPendingInput;

    using Loss = MuxLoss;

    /// Attaches to session @p name: when @p takeover, also when another window
    /// shows it, which then detaches. Waits until the daemon names the
    /// session's panes. Nullptr with @p error on a failure. @p sockets are
    /// the daemon's: the user's, or a test's. @p eventQueue is Mux.eventQueue
    /// in bytes.
    static std::shared_ptr<MuxClient> attach(const QString& name, bool takeover, QString* error,
                                             const AfterglowProto::MuxSockets& sockets, std::size_t eventQueue);
    ~MuxClient() override;

    const QString& session() const { return m_session; }
    /// The Mux.eventQueue that the attach passed, in bytes.
    std::size_t eventQueue() const { return m_eventQueue; }
    /// The panes of the session, as the daemon named them.
    const std::vector<std::uint32_t>& panes() const { return m_paneIds; }
    /// The window still shows the session: no end event came, and the
    /// stream works.
    bool showing() const { return !m_ended && m_stream; }

    /// @p pane shows session pane @p id from now on: its frames go to it.
    void bind(std::uint32_t id, Pane* pane);
    /// The pane that showed @p id goes. The last one ends the link: with
    /// @p kill, it also ends the session (Mux.onClose). The kill goes on this
    /// window's stream, so a window that another one took the session from
    /// ends nothing.
    void release(std::uint32_t id, bool kill);

    /// False when the link ended, or the input that waits is past
    /// MaxWaitingInput.
    bool sendInput(std::uint32_t id, std::string_view bytes);
    /// A new size replaces one that still waits: the program gets the
    /// newest, not each size of a drag.
    void sendResize(std::uint32_t id, const AfterglowMux::PaneSize& size);
    /// The window's Reset and Clear actions, which the daemon applies to
    /// the session's terminal.
    void sendReset(std::uint32_t id);
    void sendClear(std::uint32_t id);
    /// Whether the user can see pane @p id: its terminal in the daemon
    /// reports a change to the program (mode 2033).
    void sendVisible(std::uint32_t id, bool visible);
    /// The input that waits for the channel, in bytes: the backlog that a
    /// file transfer or a drag holds back for. Any thread.
    std::size_t pendingInput() const { return m_waitingBytes.load(std::memory_order_relaxed); }
    /// The program in the foreground of the session, for the question before
    /// a close. Empty when none runs, or when the daemon does not answer.
    /// It calls the daemon and waits for the answer.
    QString foregroundJob() const;
    /// The working directory of the program of pane @p id, as the system has
    /// it: for a shell that does not report its own (OSC 7). Empty when the
    /// daemon does not answer. It calls the daemon and waits for the answer.
    QString directory(std::uint32_t id) const;
    /// Reads the stream for up to @p waitMs, and handles what comes, until
    /// @p done: a pane that waits for the answer of a host call on the GUI
    /// thread, which alone reads the stream (a drag's answer wait). True
    /// when @p done.
    bool waitForHostCalls(int waitMs, const std::function<bool()>& done);

private:
    MuxClient() = default;
    void readable();
    void writable();
    void handleFrames();
    /// Applies the frames that wait for pane @p id. False when one does not
    /// apply: the link is lost.
    bool readState(std::uint32_t id);
    /// The pane's program exited: the pane hears it once its last frame and
    /// the PaneClosed event both came.
    void paneExited(std::uint32_t id);
    /// A Host event: a call, or a part of one. False for one that the window
    /// cannot take. @p lostLink is set when a frame of the pane, which goes
    /// first, did not apply: the link is lost.
    bool hostEvent(AfterglowMux::Attach::Event& event, bool* lostLink);
    /// A call for pane @p id, @p bytes encoded: its pane answers it, or it
    /// waits for the pane. False when the calls that wait would pass
    /// MaxWaitingHostCalls.
    bool hostCall(std::uint32_t id, AfterglowMux::Host::Call call, std::size_t bytes);
    void queueInput(std::string frame, bool resize);
    void sendWaitingInput();
    void updateNotifiers();
    void end(Loss loss);
    void lost();

    QString m_session;
    std::size_t m_eventQueue = 0;
    AfterglowProto::MuxSockets m_sockets; ///< the daemon's, for the job
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_input = nullptr;
    PhosphorChannel::StreamWriter* m_actions = nullptr; ///< Attach::ActionChannel: the kill
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    std::vector<std::uint32_t> m_paneIds;
    std::map<std::uint32_t, std::uint32_t> m_channels; ///< the state channel of each pane
    std::map<std::uint32_t, QPointer<Pane>> m_panes;
    std::map<std::uint32_t, int> m_exits; ///< PaneClosed came: the wait status
    /// The calls for panes that the window has not made yet, and their bytes.
    struct WaitingHostCalls
    {
        std::vector<AfterglowMux::Host::Call> calls;
        std::size_t bytes = 0;
    };
    std::map<std::uint32_t, WaitingHostCalls> m_waitingHostCalls;
    std::size_t m_waitingHostBytes = 0;
    /// The most bytes of calls that wait for panes: what the daemon holds
    /// for the window at most.
    static constexpr std::size_t MaxWaitingHostCalls = std::size_t(AfterglowMux::MaxEventQueue) * 2 + AfterglowMux::Host::MaxCall;
    std::map<std::uint32_t, std::string> m_hostParts; ///< the parts of a call that came, by pane
    std::set<std::uint32_t> m_closedChannels; ///< the pane's last frame was read
    std::deque<std::string> m_waitingInput;
    /// Where m_waitingInput holds the newest size of a pane, while nothing
    /// follows it: a later size takes its place.
    std::optional<std::uint32_t> m_waitingResizeOf;
    std::atomic<std::size_t> m_waitingBytes{0};
    /// The session ended, another window took it, the window detached, or the
    /// link broke.
    bool m_ended = false;
    Loss m_loss = Loss::Broken; ///< why, once m_ended
};

} // namespace Afterglow
