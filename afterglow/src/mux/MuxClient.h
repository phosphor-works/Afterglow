// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxConnect.h"
#include "MuxHost.h"
#include "MuxLayout.h"
#include "MuxProtocol.h"
#include "MuxStream.h"

#include <AfterglowProto/Json.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowPty/Pty.h>

#include <QJsonArray>
#include <QJsonValue>
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

/// What a session pane that the window closes ends (MuxClient::release).
enum class MuxRelease : std::uint8_t {
    Pane, ///< its program: the user closed the pane, and the window stays
    Session, ///< the session: the window closes, and Mux.onClose is kill
    Nothing, ///< nothing: the window closes, and Mux.onClose is detach, or Afterglow quits
};

/// A window's link to a session of the mux daemon: the stream that the attach
/// call opens (MuxStream.h). The session's tabs and splits come first, and
/// the window lays them out (layout()). The state of each pane of the session
/// comes in frames, which the Pane applies to its replica terminal. The calls
/// of the session's terminal to its host come as events, and the pane answers
/// them (MuxHost.h). The pane's input and answers go back, and so do the
/// window's layout and the panes that the user closes. It runs on the GUI
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
    /// session's layout. Nullptr with @p error on a failure. @p route leads
    /// to the daemon: the user's on this computer or on another host
    /// (`aglow-mux proxy` over ssh), or a test's. @p eventQueue is
    /// Mux.eventQueue in bytes, and @p history Terminal.scrollback, the
    /// history rows that the window keeps of each pane.
    static std::shared_ptr<MuxClient> attach(const QString& name, bool takeover, QString* error, const AfterglowMux::Route& route,
                                             std::size_t eventQueue, std::size_t history);
    ~MuxClient() override;

    const QString& session() const { return m_session; }
    /// The ssh destination of the session's daemon, empty for this computer.
    QString host() const { return QString::fromStdString(m_route.host); }
    /// The session runs on another host: its paths are not this computer's.
    bool remote() const { return !m_route.proxy.empty(); }
    /// The Mux.eventQueue that the attach passed, in bytes.
    std::size_t eventQueue() const { return m_eventQueue; }
    /// The panes of the session, as the daemon named them.
    const std::vector<std::uint32_t>& panes() const { return m_paneIds; }
    /// The session's tabs and splits when the window attached: the daemon
    /// sends them first.
    const AfterglowMux::Layout& layout() const { return m_layout; }
    /// The window still shows the session: no end event came, and the
    /// stream works.
    bool showing() const { return !m_ended && m_stream; }

    /// @p pane shows session pane @p id from now on: its frames go to it.
    void bind(std::uint32_t id, Pane* pane);
    /// The pane that showed @p id goes, and with MuxRelease::Pane its program
    /// ends in the daemon. The last one ends the link: when any pane went
    /// with MuxRelease::Session, it also ends the session (Mux.onClose),
    /// whichever pane of the window goes last. Both go on this window's
    /// stream, so a window that another one took the session from ends
    /// nothing.
    void release(std::uint32_t id, MuxRelease release);
    /// Starts a pane in the session with @p launch at @p size, for a new tab
    /// or split of the window: its ID, which the window binds a pane to, or
    /// nothing with @p error. It calls the daemon and waits for the answer,
    /// as the start of a program of the window waits for its exec.
    std::optional<std::uint32_t> newPane(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size, QString* error) const;
    /// The window's tabs and splits after a change: the daemon keeps them
    /// for the next window. A layout like the last one sent does not go
    /// again.
    void sendLayout(const AfterglowMux::Layout& layout);
    /// Moves the panes of @p layout to a new session of the daemon, which
    /// keeps that layout (Fn::BreakOut): the name of the new session, or
    /// nothing with @p error. The window then closes those panes with
    /// MuxRelease::Nothing. It calls the daemon and waits for the answer.
    std::optional<QString> breakOut(const AfterglowMux::Layout& layout, QString* error) const;

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
    /// The program in the foreground of pane @p id, for the question before a
    /// close. Empty when none runs, when the session ended, or when the
    /// daemon does not answer. It calls the daemon and waits for the answer.
    QString paneJob(std::uint32_t id) const;
    /// The working directory of the program of pane @p id, as the system has
    /// it (Fn::PaneDirectory): for a shell that reported no directory that
    /// exists on this machine. Empty when the session ended, or when the
    /// daemon does not answer. It calls the daemon and waits for the answer.
    QString directory(std::uint32_t id) const;
    /// What the window answers to a call of the control target that the
    /// daemon passes it: a result, or an error.
    struct ControlAnswer
    {
        QJsonValue result;
        QString error;
    };
    /// Answers the calls of `afterglow ctl` for the session that only a
    /// window can answer (Attach::EventType::Control), with the daemon's pane
    /// IDs. On the GUI thread, after the frames and events that came first.
    using ControlHandler = std::function<ControlAnswer(const QString& fn, const QJsonArray& args)>;
    void setControlHandler(ControlHandler handler) { m_controlHandler = std::move(handler); }

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
    /// A call for pane @p id, @p bytes encoded, that the daemon sent after
    /// @p frames frames of the pane: its pane answers it once the window
    /// made the pane and read as many, and it waits until then. False when
    /// the calls that wait would pass MaxWaitingHostCalls.
    bool hostCall(std::uint32_t id, AfterglowMux::Host::Call call, std::size_t bytes, std::uint64_t frames);
    /// Makes the calls of pane @p id whose frames the window read, in their
    /// order.
    void releaseHostCalls(std::uint32_t id);
    void queueInput(std::string frame, bool resize);
    /// Queues @p input for pane @p id after the input that waits, while the
    /// session shows.
    void sendPaneInput(std::uint32_t id, AfterglowMux::Attach::Input& input);
    /// One call of the daemon's on a connection of its own, while the session
    /// shows: the GUI thread waits for the answer. A failure is logged and
    /// gives nothing.
    std::optional<AfterglowProto::Json::Value> callDaemon(const char* fn, AfterglowProto::Json::Array args,
                                                          QString* error = nullptr) const;
    void sendWaitingInput();
    /// Answers the calls of the control target that came (m_controlCalls).
    /// Out of handleFrames(): an answer makes panes, which read the stream.
    void answerControlCalls();
    void updateNotifiers();
    void end(Loss loss);
    void lost();

    QString m_session;
    std::size_t m_eventQueue = 0;
    AfterglowMux::Route m_route; ///< to the daemon, for callDaemon
    /// The proxy command that carries the stream from another host: it ends
    /// after the stream.
    std::shared_ptr<AfterglowMux::ProxyProcess> m_proxy;
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_input = nullptr;
    PhosphorChannel::StreamWriter* m_actions = nullptr; ///< Attach::ActionChannel: the kill
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    std::vector<std::uint32_t> m_paneIds;
    AfterglowMux::Layout m_layout;
    bool m_layoutCame = false;
    std::string m_sentLayout; ///< the JSON of the last layout that went
    std::map<std::uint32_t, std::uint32_t> m_channels; ///< the state channel of each pane
    std::map<std::uint32_t, QPointer<Pane>> m_panes;
    /// The panes that the user closed while the session runs: their frames
    /// are read and dropped until their channel closes, which frees its ID.
    std::set<std::uint32_t> m_released;
    std::map<std::uint32_t, int> m_exits; ///< PaneClosed came: the wait status
    /// The calls for panes that the window has not made yet, or whose frames
    /// before them did not all come, and their bytes.
    struct WaitingHostCall
    {
        AfterglowMux::Host::Call call;
        std::uint64_t frames = 0; ///< of the pane, before the call
        std::size_t bytes = 0;
    };
    struct WaitingHostCalls
    {
        std::deque<WaitingHostCall> calls;
        std::size_t bytes = 0;
    };
    std::map<std::uint32_t, std::uint64_t> m_framesRead; ///< the frames of each pane that the window read
    std::map<std::uint32_t, WaitingHostCalls> m_waitingHostCalls;
    std::size_t m_waitingHostBytes = 0;
    /// The most bytes of calls that wait for panes: what the daemon holds
    /// for the window at most, and those that left it and wait for frames on
    /// their way, which the windows of the channels bound.
    static constexpr std::size_t MaxWaitingHostCalls = std::size_t(AfterglowMux::MaxEventQueue) * 2 + AfterglowMux::Host::MaxCall;
    std::map<std::uint32_t, std::string> m_hostParts; ///< the parts of a call that came, by pane
    std::set<std::uint32_t> m_closedChannels; ///< the pane's last frame was read
    std::deque<std::string> m_waitingInput;
    std::deque<std::string> m_waitingActions; ///< ClosePane frames that wait for the action channel
    struct ControlCall
    {
        qint64 id = 0; ///< 0: the daemon wants no answer
        QString fn;
        QJsonArray args;
    };
    std::deque<ControlCall> m_controlCalls;
    bool m_controlCallsQueued = false;
    ControlHandler m_controlHandler;
    /// Where m_waitingInput holds the newest size of a pane, while nothing
    /// follows it: a later size takes its place.
    std::optional<std::uint32_t> m_waitingResizeOf;
    std::atomic<std::size_t> m_waitingBytes{0};
    /// The session ended, another window took it, the window detached, or the
    /// link broke.
    bool m_ended = false;
    bool m_streamGone = false; ///< lost(): nothing more comes, also not the rest of an exit
    Loss m_loss = Loss::Broken; ///< why, once m_ended
    bool m_lossShown = false; ///< a pane said why the window lost the session: the others say nothing
    /// A pane went with MuxRelease::Session: the window closes, and the last
    /// pane to go ends the session. Panes that left the session before
    /// (breakOut) can wait for their delete and go after it.
    bool m_killOnEnd = false;
};

} // namespace Afterglow
