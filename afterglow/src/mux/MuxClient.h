// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxCommands.h"
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

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QSocketNotifier;

namespace PhosphorChannel {
class Stream;
class StreamWriter;
} // namespace PhosphorChannel

namespace Afterglow {

/// The connection that answered an attach call: it carries the session's
/// stream from then on. It closes unless MuxClient::adopt takes it.
struct SessionLink
{
    /// First: it goes after the connection, which ends it.
    std::shared_ptr<AfterglowMux::ProxyProcess> proxy;
    int fd = -1;
    /// What the attach answered (Fn::Attach), for MuxClient::environment()
    /// and the window's own calls.
    std::vector<std::pair<std::string, std::string>> environment;
    std::uint64_t window = 0;

    SessionLink() = default;
    SessionLink(SessionLink&& other) noexcept
        : proxy(std::move(other.proxy))
        , fd(std::exchange(other.fd, -1))
        , environment(std::move(other.environment))
        , window(other.window)
    {
    }
    SessionLink& operator=(SessionLink&& other) noexcept;
    SessionLink(const SessionLink&) = delete;
    SessionLink& operator=(const SessionLink&) = delete;
    ~SessionLink();
};

/// The first half of MuxClient::attach(): the calls that wait for the
/// daemon (its connection, maybe over ssh, and the attach call), with no Qt
/// object, for any thread. Nothing, with @p error, when they fail.
std::optional<SessionLink> attachSession(const QString& name, bool takeover, const AfterglowMux::Route& route, std::size_t eventQueue,
                                         std::size_t history, QString* error);

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
    /// nothing: the window closes, and Mux.onClose is detach, or Afterglow
    /// quits, or the pane moved to a session of its own (breakOut)
    Nothing,
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
    /// The most input of one pane that waits while the channel is full, in
    /// bytes: as much as a pane's own PTY queues. Input past it is refused,
    /// as a PTY refuses it. Each pane has its own: a paste into one pane
    /// refuses no key of another.
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
    /// The client of the connection of @p link, which answered the attach
    /// call of session @p name (attachSession): the second half of attach(),
    /// on the GUI thread. Waits until the daemon names the layout.
    static std::shared_ptr<MuxClient> adopt(const QString& name, SessionLink link, QString* error, const AfterglowMux::Route& route,
                                            std::size_t eventQueue);
    ~MuxClient() override;

    const QString& session() const { return m_session; }
    /// The ssh destination of the session's daemon, empty for this computer.
    QString host() const { return QString::fromStdString(m_route.host); }
    /// The session runs on another host: its paths are not this computer's.
    bool remote() const { return !m_route.proxy.empty(); }
    /// The Mux.eventQueue that the attach passed, in bytes.
    std::size_t eventQueue() const { return m_eventQueue; }
    /// The environment that the session's panes start from: the one of the
    /// client that made the session (`afterglow mux new`), which a new pane's
    /// launch changes as the first pane's did. Not this process's.
    const std::vector<std::pair<std::string, std::string>>& environment() const { return m_environment; }
    /// The session's tabs and splits when the window attached: the daemon
    /// sends them first.
    const AfterglowMux::Layout& layout() const { return m_layout; }
    /// The window still shows the session: no end event came, and the
    /// stream works.
    bool showing() const { return !m_ended && m_stream; }

    /// @p pane shows session pane @p id from now on: its frames go to it.
    void bind(std::uint32_t id, Pane& pane);
    /// The pane that showed @p id goes, and with MuxRelease::Pane its program
    /// ends in the daemon. A pane that moved out (breakOut) left already. The
    /// last one ends the link: when any pane went
    /// with MuxRelease::Session, it also ends the session (Mux.onClose),
    /// whichever pane of the window goes last. Both go on this window's
    /// stream, so a window that another one took the session from ends
    /// nothing.
    void release(std::uint32_t id, MuxRelease release);
    /// Starts a pane in the session with @p launch at @p size, for a new tab
    /// or split of the window: its ID, which the window binds a pane to at
    /// once, or nothing with @p error. It calls the daemon and waits for the
    /// answer, as the start of a program of the window waits for its exec. A
    /// pane that the daemon names, and that the window neither asked for nor
    /// binds (a call that timed out after the daemon started it), ends.
    std::optional<std::uint32_t> newPane(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size, QString* error);
    /// The window's tabs and splits after a change: the daemon keeps them
    /// for the next window. A layout like the last one sent does not go
    /// again.
    void sendLayout(const AfterglowMux::Layout& layout);
    /// The settings of the window, after its attach and after a change: the
    /// session follows them (AfterglowMux::SessionSettings). Settings like
    /// the last ones sent do not go again.
    void sendSettings(const AfterglowMux::SessionSettings& settings);
    /// Moves the panes of @p layout to a new session of the daemon, which
    /// keeps that layout (Fn::BreakOut): the name of the new session, or
    /// nothing with @p error. Those panes leave the link at once: no frame
    /// or exit of theirs reaches them, and the window closes them with
    /// MuxRelease::Nothing. The calls of their terminals that reach this
    /// window still get the answers of a pane that no window shows
    /// (Host::answerWithoutWindow), in their order: the daemon answers those
    /// that did not leave it at once, so those answers can come first. It
    /// calls the daemon and waits for the answer.
    std::optional<QString> breakOut(const AfterglowMux::Layout& layout, QString* error);

    /// False when the link ended, or the input of pane @p id that waits
    /// would pass MaxWaitingInput.
    bool sendInput(std::uint32_t id, std::string_view bytes);
    /// A new size replaces one that still waits: the program gets the
    /// newest, not each size of a drag. The size that the pane has in the
    /// daemon already (the last one sent, or the one of newPane) does not go.
    void sendResize(std::uint32_t id, const AfterglowMux::PaneSize& size);
    /// The window's Reset and Clear actions, which the daemon applies to
    /// the session's terminal.
    void sendReset(std::uint32_t id);
    void sendClear(std::uint32_t id);
    /// Whether the user can see pane @p id: its terminal in the daemon
    /// reports a change to the program (mode 2033), and the daemon holds the
    /// screens of a pane that the user cannot see (StateWriter::updateHeld)
    /// until it shows, or until a Refresh (askRefresh()). It goes on the
    /// action channel, past the input that waits for a program.
    void sendVisible(std::uint32_t id, bool visible);
    /// The input of pane @p id that waits for the channel, in bytes: the
    /// backlog that a file transfer or a drag of that pane holds back for.
    /// Any thread.
    std::size_t pendingInput(std::uint32_t id) const;
    /// The program in the foreground of pane @p id, for the question before a
    /// close. Empty when none runs, when the session ended, or when the
    /// daemon does not answer. It calls the daemon and waits for the answer.
    QString paneJob(std::uint32_t id) const;
    /// paneJob for each pane of @p ids, in their order, on one connection:
    /// a daemon that does not answer one is asked nothing more, so the GUI
    /// thread waits for it once, not once for each pane.
    std::vector<QString> paneJobs(const std::vector<std::uint32_t>& ids) const;
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
        /// The error's code, as the control target of a window sends it
        /// (`no_pane`, `denied`): empty for CALL_FAILED.
        QString code{};
    };
    /// Sends the answer to a call of the control target, once.
    using ControlReply = std::function<void(const ControlAnswer& answer)>;
    /// Answers the calls of `afterglow ctl` for the session that only a
    /// window can answer (Attach::EventType::Control), with the daemon's pane
    /// IDs, through @p reply: at once, or later (openProject, overlay,
    /// screenshot). @p client is the daemon's number of the client of the
    /// call: its overlays and their inputs are its own. On the GUI thread,
    /// after the frames and events that came first.
    using ControlHandler = std::function<void(const QString& fn, const QJsonArray& args, qint64 client, ControlReply reply)>;
    void setControlHandler(ControlHandler handler) { m_controlHandler = std::move(handler); }
    /// Serves the command clients of the session's panes (`aglow run`) with
    /// @p host: the socket of the Afterglow whose commands answer them, and
    /// the window's IDs of the panes. A session of another host gets none.
    void setCommandsHost(MuxCommands::Host host);

    /// Reads the stream for up to @p waitMs, and handles what comes, until
    /// @p done: a pane that waits for the answer of a host call on the GUI
    /// thread, which alone reads the stream (a drag's answer wait). True
    /// when @p done.
    bool waitForHostCalls(int waitMs, const std::function<bool()>& done);
    /// How long a refresh waits: as long as a call of the daemon, which a
    /// daemon that does not answer one does not answer either.
    static constexpr int RefreshWaitMs = AfterglowMux::CallTimeoutMs;
    /// Asks the daemon for the screens and the history of pane @p id as they
    /// are now (Attach::InputType::Refresh), also of a pane that the window
    /// does not show, whose screens the daemon holds, and waits for nothing.
    /// Once the answer came and the frames that it names applied, the pane
    /// hears it (Pane::sessionRefreshed), with the ticket that this returns.
    /// 0 when nothing comes to wait for: the pane has no channel yet, its
    /// last frame came, or the session no longer shows.
    std::uint64_t askRefresh(std::uint32_t id);
    /// Reads the stream until the answers of @p tickets (pane, ticket of
    /// askRefresh()) came and their frames applied, for up to @p waitMs:
    /// before a read of the panes' text, or a capture. An answer to a later
    /// Refresh of the pane counts too. The calls of the panes' terminals
    /// that come meanwhile wait until after it, so that the read runs no
    /// answer of the window, unless they would pass what the window holds of
    /// them: then the panes answer them meanwhile, and the link stays. A
    /// pane whose last frame came has nothing more to wait for. Each pane
    /// hears of its own answer, also when another one does not come in
    /// time. True when all came.
    bool waitRefreshed(const std::map<std::uint32_t, std::uint64_t>& tickets, int waitMs = RefreshWaitMs);

private:
    MuxClient() = default;
    void readable();
    void writable();
    void handleFrames();
    /// Answers @p call of pane @p id, which went to another session, as a
    /// pane that no window shows: its program still waits for an answer.
    void answerMoved(std::uint32_t id, const AfterglowMux::Host::Call& call);
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
    /// Pane @p id is no longer the window's: its frames are read and dropped
    /// until its channel closes, and its calls go nowhere.
    void letGo(std::uint32_t id);
    /// Queues @p frame after the input that waits: pane @p id's input, or for
    /// 0 a frame of the window's own, which counts against no pane: its
    /// layout with @p layout, its settings and its answers to the daemon.
    void queueInput(std::string frame, std::uint32_t id, bool layout = false);
    /// Adds @p bytes to (or with @p remove takes them from) the input of
    /// pane @p id that waits.
    void countWaiting(std::uint32_t id, std::size_t bytes, bool remove);
    /// Queues @p input for pane @p id after the input that waits, while the
    /// session shows.
    void sendPaneInput(std::uint32_t id, AfterglowMux::Attach::Input& input);
    /// Queues @p input, an action (Attach::isAction), for the action channel.
    void sendAction(const AfterglowMux::Attach::Input& input);
    /// Tells each pane whose Refresh got its answer, and whose frames that
    /// the answer names applied, its newest ticket (Pane::sessionRefreshed).
    void tellRefreshed();
    /// One call of the daemon's on a connection of its own, while the session
    /// shows: the GUI thread waits for the answer. A failure is logged and
    /// gives nothing, with the daemon's words in @p error.
    std::optional<AfterglowProto::Json::Value> callDaemon(const char* fn, AfterglowProto::Json::Array args,
                                                          QString* error = nullptr) const;
    void sendWaitingInput();
    /// Answers the calls of the control target that came (m_controlCalls).
    /// Out of handleFrames(): an answer makes panes, which read the stream.
    void answerControlCalls();
    /// Sends @p answer to call @p id of the control target.
    void sendControlReply(qint64 id, const ControlAnswer& answer);
    void updateNotifiers();
    void end(Loss loss);
    void lost();

    QString m_session;
    std::size_t m_eventQueue = 0;
    AfterglowMux::Route m_route; ///< to the daemon, for callDaemon
    /// The proxy command that carries the stream from another host: it ends
    /// after the stream.
    std::shared_ptr<AfterglowMux::ProxyProcess> m_proxy;
    /// The serial of the window in the attach answer: its newPane and
    /// breakOut give it, so that the daemon knows them from those of a
    /// window that another one took the session from.
    std::uint64_t m_window = 0;
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_input = nullptr;
    /// Attach::ActionChannel: the kill, the panes that the user closed, what
    /// the user sees of each pane (Visible) and the reads of a pane's text
    /// (Refresh)
    PhosphorChannel::StreamWriter* m_actions = nullptr;
    /// The command clients of the session's panes, on Attach::CommandsChannel.
    std::unique_ptr<MuxCommands> m_commands;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    std::vector<std::pair<std::string, std::string>> m_environment; ///< environment()
    AfterglowMux::Layout m_layout;
    bool m_layoutCame = false;
    std::string m_sentLayout; ///< the JSON of the last layout that went, or that came
    std::string m_sentSettings; ///< the JSON of the last settings that went
    std::map<std::uint32_t, std::uint32_t> m_channels; ///< the state channel of each pane
    std::map<std::uint32_t, QPointer<Pane>> m_panes;
    /// The panes that the window binds next: those of the layout that came,
    /// those that newPane started, and those that the daemon places for
    /// `afterglow ctl` (placePane). A pane that the daemon opens and that
    /// is none of these, nor bound, nor released, is nobody's.
    std::set<std::uint32_t> m_unbound;
    /// The panes that the window let go while the session runs: their frames
    /// are read and dropped until their channel closes, which frees its ID.
    /// A pane that the daemon never opened a channel for (it closed before
    /// it had one) stays: the daemon never gives its ID again.
    std::set<std::uint32_t> m_released;
    /// Those of m_released that went to another session (breakOut): their
    /// calls that reach this window get answerMoved().
    std::set<std::uint32_t> m_movedOut;
    /// The size that each pane has in the daemon: the last that went, or the
    /// one that newPane started it with.
    std::map<std::uint32_t, AfterglowMux::PaneSize> m_sentSizes;
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
    /// The Refresh inputs of a pane and their Refreshed events, which come
    /// in the same order: the newest names the frames that hold the screens.
    struct Refreshes
    {
        std::uint64_t asked = 0;
        std::uint64_t answered = 0;
        std::uint64_t frames = 0; ///< of the newest Refreshed
        std::uint64_t told = 0; ///< the newest answer that the pane heard of (tellRefreshed)
    };
    std::map<std::uint32_t, Refreshes> m_refreshes;
    /// waitRefreshed() reads the stream: the calls of the panes wait (releaseHostCalls).
    bool m_holdingCalls = false;
    std::map<std::uint32_t, WaitingHostCalls> m_waitingHostCalls;
    std::size_t m_waitingHostBytes = 0;
    /// The most bytes of calls that wait for panes: what the daemon holds
    /// for the window at most, and those that left it and wait for frames on
    /// their way, which the windows of the channels bound.
    static constexpr std::size_t MaxWaitingHostCalls = std::size_t(AfterglowMux::MaxEventQueue) * 2 + AfterglowMux::Host::MaxCall;
    std::map<std::uint32_t, std::string> m_hostParts; ///< the parts of a call that came, by pane
    /// The pane's channel was read to Closed: its ID may be the channel of
    /// another pane now, so it is never read again. A bound pane whose exit
    /// came whole stays here until the window lets it go.
    std::set<std::uint32_t> m_closedChannels;
    struct WaitingInput
    {
        std::string frame;
        std::uint32_t pane = 0; ///< whose input it is: 0 for the window's own
        bool layout = false; ///< the layout, which a newer one replaces
    };
    /// One queue for the control channel: the daemon reads it in order. At
    /// most one layout waits, the newest.
    std::deque<WaitingInput> m_waitingInput;
    std::deque<std::string> m_waitingActions; ///< the frames of actions that wait for the action channel
    struct ControlCall
    {
        qint64 id = 0; ///< 0: the daemon wants no answer
        QString fn;
        QJsonArray args;
        qint64 client = 0; ///< the daemon's number of the client of the call, 0 for the daemon's own
    };
    std::deque<ControlCall> m_controlCalls;
    bool m_controlCallsQueued = false;
    ControlHandler m_controlHandler;
    /// Where m_waitingInput holds the newest size of a pane, while nothing
    /// follows it: a later size takes its place.
    std::optional<std::uint32_t> m_waitingResizeOf;
    /// The bytes of each pane's input in m_waitingInput, which other threads
    /// read (pendingInput).
    mutable std::mutex m_waitingMutex;
    std::map<std::uint32_t, std::size_t> m_waitingBytes;
    /// A call of readable() waits in the event loop: a write of the window's
    /// own found the stream broken, and readFd() may stay quiet.
    bool m_readQueued = false;
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
