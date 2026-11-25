// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxCommands.h"
#include "MuxConnect.h"
#include "MuxHost.h"
#include "MuxLayout.h"
#include "MuxProtocol.h"
#include "MuxStream.h"

#include <AfterglowProto/Json.h>
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
    bool agent = false; ///< an agent drives the session (SessionOptions::agent)
    /// The way that reached the daemon, which the window's calls take too.
    AfterglowMux::Route route;

    SessionLink() = default;
    SessionLink(SessionLink&& other) noexcept
        : proxy(std::move(other.proxy))
        , fd(std::exchange(other.fd, -1))
        , environment(std::move(other.environment))
        , window(other.window)
        , agent(other.agent)
        , route(std::move(other.route))
    {
    }
    SessionLink& operator=(SessionLink&& other) noexcept;
    SessionLink(const SessionLink&) = delete;
    SessionLink& operator=(const SessionLink&) = delete;
    ~SessionLink();
};

/// The first half of MuxClient::attach(): the calls that wait for the
/// daemon (its connection, maybe over ssh, and the attach call), with no Qt
/// object, for any thread. @p signInWaitMs bounds the first answer through a
/// proxy route, for which ssh may first sign in (Mux.signInTimeout). A proxy
/// route runs ssh in @p environment, and without one in the environment of
/// the process, which the GUI thread changes (an activation token): a caller
/// off the GUI thread passes a copy taken there
/// (AfterglowMux::currentEnvironment()). Nothing, with @p error, when they
/// fail.
std::optional<SessionLink> attachSession(const QString& name, bool takeover, QString* error,
                                         const AfterglowMux::Route& route, std::size_t eventQueue, std::size_t history,
                                         int signInWaitMs = AfterglowMux::CallTimeoutMs,
                                         const AfterglowMux::Environment* environment = nullptr);

class Pane;

/// Why the window no longer shows the session.
enum class MuxLoss : std::uint8_t {
    Ended, ///< the session ended (`afterglow mux kill`, or its last program)
    Detached, ///< `afterglow mux detach`
    TakenOver, ///< another window shows the session now
    Broken, ///< the daemon ended or failed, or sent a frame that does not apply
    /// the pane went to a session of its own (MuxClient::breakOutLater) or to
    /// another session (joinLater), but the answer of the daemon did not come
    /// in time; or `afterglow ctl --session` moved it to another session
    MovedOut,
};

/// What a session pane that the window closes ends (MuxClient::release).
enum class MuxRelease : std::uint8_t {
    Pane, ///< its program: the user closed the pane, and the window stays
    /// the session: the window closes, and Mux.onClose is kill
    /// (TerminalWindow::windowCloseRelease)
    Session,
    /// nothing: the window closes and keeps the session (Mux.onClose detach,
    /// a project's or an agent's session), or Afterglow quits, or the pane
    /// moved to a session of its own (breakOutLater)
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

Q_SIGNALS:
    /// The session became an agent session (agent()).
    void agentChanged();

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
    /// history rows that the window keeps of each pane. @p signInWaitMs as
    /// attachSession() takes it. The later calls of the window share the
    /// connection that the stream's ssh signed in, and wait no longer than
    /// a call.
    static std::shared_ptr<MuxClient> attach(const QString& name, bool takeover, QString* error,
                                             const AfterglowMux::Route& route, std::size_t eventQueue,
                                             std::size_t history, int signInWaitMs = AfterglowMux::CallTimeoutMs);
    /// The client of the connection of @p link, which answered the attach
    /// call of session @p name (attachSession), over the link's route: the
    /// second half of attach(), on the GUI thread. Waits until the daemon
    /// names the layout.
    static std::shared_ptr<MuxClient> adopt(const QString& name, SessionLink link, QString* error,
                                            std::size_t eventQueue);
    ~MuxClient() override;

    const QString& session() const
    {
        return m_session;
    }
    /// The ssh destination of the session's daemon, empty for this computer.
    QString host() const
    {
        return QString::fromStdString(m_route.host);
    }
    /// The session runs on another host: its paths are not this computer's.
    bool remote() const
    {
        return !m_route.proxy.empty();
    }
    /// The Mux.eventQueue that the attach passed, in bytes.
    std::size_t eventQueue() const
    {
        return m_eventQueue;
    }
    /// The environment that the session's panes start from, which a new
    /// pane's launch changes as the first pane's did: the one of the client
    /// that made the session (`afterglow mux new`), of the Afterglow that
    /// opened a project into it, or for a session that a break out made, the
    /// one of the session that the panes left. Not this process's.
    const std::vector<std::pair<std::string, std::string>>& environment() const
    {
        return m_environment;
    }
    /// An agent drives the session (`afterglow mcp`): the attach said so, or
    /// the daemon's call "agent" when an agent adopted it later.
    bool agent() const
    {
        return m_agent;
    }
    /// The session's tabs and splits when the window attached: the daemon
    /// sends them first.
    const AfterglowMux::Layout& layout() const
    {
        return m_layout;
    }
    /// The window still shows the session: no end event came, and the
    /// stream works.
    bool showing() const
    {
        return !m_ended && m_stream;
    }
    /// What the stream told of session pane @p id so far: its PaneOpened,
    /// its PaneClosed and the end of its channel. A test orders these
    /// against the answer of a call (newPaneLater, breakOutLater) with it.
    struct Heard
    {
        bool opened = false;
        bool exited = false;
        bool channelEnded = false;
    };
    Heard heard(std::uint32_t id) const
    {
        return {m_channels.contains(id), m_exits.contains(id), m_closedChannels.contains(id)};
    }

    /// @p pane shows session pane @p id from now on: its frames go to it.
    void bind(std::uint32_t id, Pane& pane);
    /// The pane that showed @p id goes, and with MuxRelease::Pane its program
    /// ends in the daemon. A pane that moved out (breakOutLater) left already. The
    /// last one ends the link: when any pane went
    /// with MuxRelease::Session, it also ends the session
    /// (TerminalWindow::windowCloseRelease),
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
    std::optional<std::uint32_t> newPane(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size,
                                         QString* error);
    /// The end of newPaneLater(): the daemon's ID of the new pane, which the
    /// window binds a pane to at once, or nothing with @p error.
    using PaneStarted = std::function<void(std::optional<std::uint32_t> pane, const QString& error)>;
    /// newPane() with the call off the GUI thread, which goes on meanwhile:
    /// a refusal at once, else empty, and @p done gets the outcome later on
    /// this thread, once, also when this object goes first. Until the
    /// answer, a pane that the daemon opens and that the window did not ask
    /// for waits for it (m_unclaimed), as its PaneOpened can come first.
    /// With @p directoryOf, a launch with no directory starts in the
    /// directory of that pane, which the daemon gives (directory()) on the
    /// same thread: for a session of another host.
    QString newPaneLater(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size, PaneStarted done,
                         std::optional<std::uint32_t> directoryOf = std::nullopt);
    /// The window binds no pane to @p id, which newPaneLater() gave (it
    /// closes, or its tab went): its program ends, and the clients of its
    /// program that wait for it find no pane. Nothing for a pane that is
    /// bound.
    void endUnbound(std::uint32_t id);

    /// Counts a thread that calls a daemon off the GUI thread while it
    /// lives (callLater(), the attach of Application, Control::offThread):
    /// made on the GUI thread before the thread starts, and moved into it as
    /// the first local of its body, so that it goes after the body's other
    /// locals. The captures go after it: they must use no static.
    class CallThread
    {
    public:
        CallThread();
        CallThread(CallThread&& other) noexcept;
        CallThread(const CallThread&) = delete;
        CallThread& operator=(const CallThread&) = delete;
        CallThread& operator=(CallThread&&) = delete;
        ~CallThread();

    private:
        bool m_counted = true;
    };
    /// Waits up to @p waitMs for the threads of CallThread to end: at quit,
    /// before the statics that they use (the logging categories) go. A
    /// reach over ssh can wait longer than a quit should. False when one
    /// still runs.
    static bool waitForCallThreads(int waitMs);
    /// The window's tabs and splits after a change: the daemon keeps them
    /// for the next window. A layout like the last one sent does not go
    /// again.
    void sendLayout(const AfterglowMux::Layout& layout);
    /// The settings of the window, after its attach and after a change: the
    /// session follows them (AfterglowMux::SessionSettings). Settings like
    /// the last ones sent do not go again.
    void sendSettings(const AfterglowMux::SessionSettings& settings);
    /// The end of breakOutLater(): the name of the new session, or nothing
    /// with @p error.
    using BrokenOut = std::function<void(std::optional<QString> session, const QString& error)>;
    /// Moves the panes of @p layout to a new session of the daemon, which
    /// keeps that layout (Fn::BreakOut), with the call off the GUI thread: a
    /// refusal at once, else empty, and @p done gets the outcome later on
    /// this thread, once, also when this object goes first. Until the
    /// answer the panes stay the window's: what the user types reaches
    /// their programs, also once they moved, and an exit of theirs (a
    /// PaneClosed of -1 once they moved) waits for the answer. With a name,
    /// those panes left the link: no frame or exit of theirs reaches them,
    /// and the window closes them with MuxRelease::Nothing. The calls of
    /// their terminals that reach this window still get the answers of a
    /// pane that no window shows (Host::answerWithoutWindow), in their
    /// order: the daemon answers those that did not leave it at once, so
    /// those answers can come first.
    QString breakOutLater(const AfterglowMux::Layout& layout, BrokenOut done);
    /// Moves session pane @p id to the session of pane @p beside, another
    /// session of the same daemon (Fn::JoinPane), with its program running:
    /// on @p placement of that pane ("left", "right", "up" or "down"), or
    /// with "new-tab" in a tab after its tab. As breakOutLater() for one
    /// pane: @p done gets the name of the session that holds the pane now.
    /// The window that shows that session places the pane. When it was the
    /// last pane of this session, the session ends.
    QString joinLater(std::uint32_t id, std::uint32_t beside, const QString& placement, BrokenOut done);
    /// The session of @p other runs under the daemon of this one: a pane can
    /// join it (joinLater). Pane IDs are those of one daemon.
    bool sameDaemon(const MuxClient& other) const;

    /// False when the link ended, or the input of pane @p id that waits
    /// would pass MaxWaitingInput.
    bool sendInput(std::uint32_t id, std::string_view bytes);
    /// The answer to pane @p id's UploadRequest call, which the daemon
    /// writes to the program in one piece (Attach::InputType::UploadAnswer
    /// and UploadAnswerPart), in order with the pane's input. False as for
    /// sendInput().
    bool sendUploadAnswer(std::uint32_t id, std::string_view bytes);
    /// A new size replaces one that still waits: the program gets the
    /// newest, not each size of a drag. The size that the pane has in the
    /// daemon already (the last one sent, or the one of newPane) does not go.
    void sendResize(std::uint32_t id, const AfterglowMux::PaneSize& size);
    /// The window's Reset and Clear actions, which the daemon applies to
    /// the session's terminal.
    void sendReset(std::uint32_t id);
    void sendClear(std::uint32_t id);
    /// The answer to a pane's DownloadBegin call, or the stop of the
    /// download whose data comes (Attach::InputType::DownloadAnswer). It
    /// goes on the action channel, past the input that waits for a program.
    void answerDownload(std::uint32_t id, AfterglowMux::Attach::DownloadReply reply);
    /// While pane @p id holds them, the window reads no more of the
    /// session's events: the download of a pane whose disk is slower than
    /// its program. The daemon then holds them, and past Mux.eventQueue it
    /// pauses the pane's output (MuxPane::downloadData). The frames of the
    /// panes that the window has still come. A pane that the window lets
    /// go holds none.
    /// The hold is of every event, of every pane: the events come in one
    /// stream, in their order, which the window keeps. Meanwhile the host
    /// calls of the other panes wait, and so do a pane that the session
    /// opens (a new split gets no frames until then), the end of a pane or
    /// of the session, the answers to Refresh, the control calls of the
    /// session and the command connections of `aglow run` in it. The
    /// writer of the download ends the hold once half its queue is on the
    /// disk.
    void holdEvents(std::uint32_t id, bool hold);
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
    /// thread waits for it once, not once for each pane. Nothing for a pane
    /// that got no answer (no daemon, no answer in time, or one asked after
    /// that), which may run a program: an empty job is an idle pane.
    std::vector<std::optional<QString>> paneJobs(const std::vector<std::uint32_t>& ids) const;
    /// The working directory of the program of pane @p id, as the system has
    /// it (Fn::PaneDirectory): for a shell that reported no directory that
    /// exists on this machine. Empty when the session ended, or when the
    /// daemon does not answer. It calls the daemon and waits for the answer.
    QString directory(std::uint32_t id) const;
    /// directory() for each pane of @p ids, in their order, on one
    /// connection, as paneJobs() asks: empty for a pane that got no answer.
    std::vector<QString> directories(const std::vector<std::uint32_t>& ids) const;
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
    /// screenshot, movePane, detachTab). @p client is the daemon's number of the client of the
    /// call: its overlays and their inputs are its own. On the GUI thread,
    /// after the frames and events that came first. Calls that come while
    /// the window no longer shows the session go unanswered: the daemon
    /// answered them when the window went.
    using ControlHandler =
        std::function<void(const QString& fn, const QJsonArray& args, qint64 client, ControlReply reply)>;
    void setControlHandler(ControlHandler handler)
    {
        m_controlHandler = std::move(handler);
    }
    /// Sends the window's layout that waits to go (TerminalWindow's delay),
    /// before each answer to a control call: the daemon reads the layout
    /// that the call changed before the answer, as `afterglow ctl ls`
    /// after `move-pane` expects.
    void setLayoutFlush(std::function<void()> flush)
    {
        m_flushLayout = std::move(flush);
    }
    /// Serves the command clients of the session's panes (`aglow run`) with
    /// @p host: the socket of the Afterglow whose commands answer them, and
    /// the window's IDs of the panes, which this client also finds for a
    /// pane that it binds before the window's splits hold it. A client of a
    /// session of another host gets a socket of RemoteSockets::endpoint(),
    /// with only the commands that a script marks remote.
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
    /// 0 when nothing comes to wait for: the window does not show the pane,
    /// the pane has no channel yet, its last frame came, or the session no
    /// longer shows. While the answer to the pane's last Refresh is late
    /// (waitRefreshed), no other goes: its ticket comes back.
    std::uint64_t askRefresh(std::uint32_t id);
    /// Reads the stream until the answers of @p tickets (pane, ticket of
    /// askRefresh()) came and their frames applied, for up to @p waitMs:
    /// before a read of the panes' text, or a capture. An answer to a later
    /// Refresh of the pane counts too. The calls of the panes' terminals
    /// that come meanwhile wait until after it, so that the read runs no
    /// answer of the window, unless they would pass what the window holds of
    /// them: then the panes answer them meanwhile, and the link stays. A
    /// pane whose last frame came has nothing more to wait for, and the
    /// calls of its terminal reach it then, also while they wait. Each pane
    /// hears of its own answer, also when another one does not come in
    /// time. An answer that did not come in time is late: until one comes,
    /// a read waits for no answer of that pane, and for none of the session
    /// (a daemon that stopped, or an ssh that stalls, would hold the GUI
    /// thread @p waitMs for each read). True when all came.
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
    /// order, unless waitRefreshed() holds them, or @p evenHeld.
    void releaseHostCalls(std::uint32_t id, bool evenHeld = false);
    /// Pane @p id is no longer the window's: its frames are read and dropped
    /// until its channel closes, and its calls go nowhere.
    void letGo(std::uint32_t id);
    /// Pane @p id is gone from every table of the window: when it went to
    /// another session, the daemon hears that it may come back.
    void forgotten(std::uint32_t id);
    /// Queues @p frame after the input that waits: pane @p id's input, or for
    /// 0 a frame of the window's own, which counts against no pane: its
    /// layout with @p layout, its settings and its answers to the daemon
    /// with @p reply.
    void queueInput(std::string frame, std::uint32_t id, bool layout = false, bool reply = false);
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
    /// gives nothing, with the daemon's words in @p error, and
    /// @p unanswered set when the call went but no answer came within
    /// @p timeoutMs: the daemon may have done it.
    std::optional<AfterglowProto::Json::Value> callDaemon(const char* fn, AfterglowProto::Json::Array args,
                                                          QString* error = nullptr,
                                                          int timeoutMs = AfterglowMux::CallTimeoutMs,
                                                          bool* unanswered = nullptr) const;
    /// What a call of the daemon brought: its result, or nothing with the
    /// daemon's words in error (never empty then), and unanswered set when
    /// the call went but no answer came in time.
    struct DaemonAnswer
    {
        std::optional<AfterglowProto::Json::Value> result;
        QString error;
        bool unanswered = false;
    };
    /// callDaemon() of session @p session over @p route, for any thread:
    /// with no Qt object, and a proxy runs in @p environment, which a
    /// caller off the GUI thread takes there.
    static DaemonAnswer callRoute(const AfterglowMux::Route& route, const QString& session, const char* fn,
                                  AfterglowProto::Json::Array args, int timeoutMs,
                                  const AfterglowMux::Environment& environment);
    /// A call of callLater() that waits for its answer: finish takes it on
    /// the GUI thread, and abandon tells its caller why it never comes when
    /// this object goes first.
    struct WaitingCall
    {
        std::function<void(DaemonAnswer answer)> finish;
        std::function<void(const QString& error)> abandon;
    };
    /// The calls of callLater(): over the route to the daemon of the
    /// session, with a proxy in the environment that it gets.
    using DaemonWork = std::function<DaemonAnswer(const AfterglowMux::Route& route, const QString& session,
                                                  const AfterglowMux::Environment& environment)>;
    /// Runs @p work (callRoute) on a thread of its own, while the session
    /// shows: @p call gets its answer on the GUI thread, once. A refusal at
    /// once, else empty.
    QString callLater(DaemonWork work, WaitingCall call);
    /// The answer of call @p id of callLater() came.
    void callAnswered(quint64 id, DaemonAnswer& answer);
    /// The arguments of Fn::BreakOut for @p layout, cut to fit a request,
    /// or nothing with @p error.
    std::optional<AfterglowProto::Json::Array> breakOutArguments(const AfterglowMux::Layout& layout,
                                                                 QString* error) const;
    /// What a breakOutLater() of @p layout makes of @p answer: the name of
    /// the new session, or nothing with @p error.
    std::optional<QString> brokeOut(const AfterglowMux::Layout& layout, const DaemonAnswer& answer, QString* error);
    /// breakOutLater() and joinLater(): the call @p fn with @p args, which
    /// moves the panes of @p layout out of the session.
    QString leaveLater(const AfterglowMux::Layout& layout, const char* fn, AfterglowProto::Json::Array args,
                       BrokenOut done);
    /// The panes of m_unclaimed end: no newPaneLater() waits any more, so
    /// none of them is the window's.
    void releaseUnclaimed();
    struct PaneAnswer
    {
        /// The daemon answered: with the value, or with a refusal (null).
        bool answered = false;
        AfterglowProto::Json::Value value;
    };
    /// Asks @p fn of each pane of @p ids, (session, pane) its arguments, on
    /// one connection, in their order. A call that failed with no code (no
    /// answer in time, the connection gone) ends the asking, so the GUI
    /// thread waits once: the panes after it get no answer.
    std::vector<PaneAnswer> askEachPane(const char* fn, const std::vector<std::uint32_t>& ids) const;
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
    bool m_agent = false;
    std::size_t m_eventQueue = 0;
    /// To the daemon, and which host the session runs on: for the window's
    /// calls (callDaemon, paneJobs), host(), remote() and the commands host.
    AfterglowMux::Route m_route;
    /// The proxy command that carries the stream from another host: it ends
    /// after the stream, off the GUI thread (AfterglowMux::endInBackground).
    std::shared_ptr<AfterglowMux::ProxyProcess> m_proxy;
    /// Reads the standard error of m_proxy while the stream lives: ssh that
    /// writes much would else block on a full pipe and stop the stream.
    QSocketNotifier* m_errorNotifier = nullptr;
    /// The most bytes of answers to control calls that wait for the
    /// daemon, which stops reading them while the input of a pane waits
    /// (Attachment::MaxHeldInput): past it, an answer is an error.
    static constexpr std::size_t MaxWaitingReplies = AfterglowMux::Attach::MaxControlReply;
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
    /// Those of m_released that went to another session (breakOutLater): their
    /// calls that reach this window get answerMoved().
    std::set<std::uint32_t> m_movedOut;
    /// The panes that hold the session's events (holdEvents).
    std::set<std::uint32_t> m_eventsHeld;
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
        /// A wait for the answer to the last Refresh timed out: no other
        /// goes, and no read waits, until the pane heard of it.
        bool late = false;
    };
    std::map<std::uint32_t, Refreshes> m_refreshes;
    /// A wait for a refresh timed out: no read waits until a pane heard of
    /// an answer.
    bool m_refreshesLate = false;
    /// waitRefreshed() reads the stream: the calls of the panes wait (releaseHostCalls).
    bool m_holdingCalls = false;
    std::map<std::uint32_t, WaitingHostCalls> m_waitingHostCalls;
    std::size_t m_waitingHostBytes = 0;
    /// The most bytes of calls that wait for panes: what the daemon holds
    /// for the window at most, and those that left it and wait for frames on
    /// their way, which the windows of the channels bound.
    static constexpr std::size_t MaxWaitingHostCalls =
        std::size_t(AfterglowMux::MaxEventQueue) * 2 + AfterglowMux::Host::MaxCall;
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
        bool reply = false; ///< a part of an answer to a control call (m_waitingReplyBytes)
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
    std::function<void()> m_flushLayout; ///< setLayoutFlush()
    /// The bytes of the answers to control calls in m_waitingInput.
    std::size_t m_waitingReplyBytes = 0;
    /// The panes of a breakOutLater whose answer did not come: the daemon may
    /// have moved them, and then their PaneClosed says -1. Such a pane
    /// leaves as MuxLoss::MovedOut, not as an exit of its program.
    std::set<std::uint32_t> m_maybeMovedOut;
    bool m_movedOutShown = false; ///< a pane of m_maybeMovedOut said why it left
    /// The panes that `afterglow ctl --session` moved to another session
    /// (the daemon's paneLeft): their PaneClosed of -1 is no exit of their
    /// programs, and they close with no word.
    std::set<std::uint32_t> m_joinedAway;
    /// The panes that went to another session (PaneClosed of -1) and that
    /// the window still knows: once it forgets one, it tells the daemon,
    /// which may then bring the pane back (InputType::PaneForgotten).
    std::set<std::uint32_t> m_leftSession;
    /// The panes of a breakOutLater() or a joinLater() whose answer did not
    /// come yet: their exit waits for it (paneExited), as they may have moved.
    std::multiset<std::uint32_t> m_leaving;
    /// The newPaneLater() calls whose answer did not come yet.
    int m_newPanesWaiting = 0;
    /// The panes that the daemon opened while a newPaneLater() waited, which
    /// no answer named yet: each is in m_unbound too, and ends once no call
    /// waits (releaseUnclaimed).
    std::set<std::uint32_t> m_unclaimed;
    /// Where the calls of callLater() give their answers: this object while
    /// it lives. A call over ssh can wait for seconds, and the window may
    /// close meanwhile.
    struct CallMailbox
    {
        explicit CallMailbox(MuxClient* owner)
            : client(owner)
        {
        }
        std::mutex mutex;
        MuxClient* client;
    };
    std::shared_ptr<CallMailbox> m_mailbox = std::make_shared<CallMailbox>(this);
    std::map<quint64, WaitingCall> m_waitingCalls; ///< the calls of callLater() by their IDs
    quint64 m_lastCall = 0;
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
    /// (breakOutLater) can wait for their delete and go after it.
    bool m_killOnEnd = false;
};

} // namespace Afterglow
