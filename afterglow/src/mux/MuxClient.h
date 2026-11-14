// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxHost.h"
#include "MuxLayout.h"
#include "MuxProtocol.h"
#include "MuxStream.h"

#include <AfterglowProto/Json.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowPty/Pty.h>

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
    /// session's layout. Nullptr with @p error on a failure. @p sockets are
    /// the daemon's: the user's, or a test's. @p eventQueue is Mux.eventQueue
    /// in bytes, and @p history Terminal.scrollback, the history rows that
    /// the window keeps of each pane.
    static std::shared_ptr<MuxClient> attach(const QString& name, bool takeover, QString* error,
                                             const AfterglowProto::MuxSockets& sockets, std::size_t eventQueue,
                                             std::size_t history);
    ~MuxClient() override;

    const QString& session() const { return m_session; }
    /// The Mux.eventQueue that the attach passed, in bytes.
    std::size_t eventQueue() const { return m_eventQueue; }
    /// The history rows of each pane that the attach asked for: the daemon
    /// sends no older rows to this link, so every replica of it keeps as
    /// many, also one made after a reload changed Terminal.scrollback.
    std::size_t history() const { return m_history; }
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
    /// Moves the panes of @p layout to a new session of the daemon, which
    /// keeps that layout (Fn::BreakOut): the name of the new session, or
    /// nothing with @p error. Those panes leave the link at once: no frame
    /// or exit of theirs reaches them, and the window closes them with
    /// MuxRelease::Nothing. It calls the daemon and waits for the answer.
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
    /// reports a change to the program (mode 2033).
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
    /// Pane @p id is no longer the window's: its frames are read and dropped
    /// until its channel closes, and its calls go nowhere.
    void letGo(std::uint32_t id);
    /// Queues @p frame after the input that waits: pane @p id's input, or
    /// the layout for 0.
    void queueInput(std::string frame, std::uint32_t id);
    /// Adds @p bytes to (or with @p remove takes them from) the input of
    /// pane @p id that waits.
    void countWaiting(std::uint32_t id, std::size_t bytes, bool remove);
    /// Queues @p input for pane @p id after the input that waits, while the
    /// session shows.
    void sendPaneInput(std::uint32_t id, AfterglowMux::Attach::Input& input);
    /// One call of the daemon's on a connection of its own, while the session
    /// shows: the GUI thread waits for the answer. A failure is logged and
    /// gives nothing, with the daemon's words in @p error.
    std::optional<AfterglowProto::Json::Value> callDaemon(const char* fn, AfterglowProto::Json::Array args,
                                                          QString* error = nullptr) const;
    void sendWaitingInput();
    void updateNotifiers();
    void end(Loss loss);
    void lost();

    QString m_session;
    std::size_t m_eventQueue = 0;
    std::size_t m_history = 0; ///< history()
    AfterglowProto::MuxSockets m_sockets; ///< the daemon's, for callDaemon
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_input = nullptr;
    /// Attach::ActionChannel: the kill and the panes that the user closed
    PhosphorChannel::StreamWriter* m_actions = nullptr;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    std::vector<std::pair<std::string, std::string>> m_environment; ///< environment()
    AfterglowMux::Layout m_layout;
    bool m_layoutCame = false;
    std::string m_sentLayout; ///< the JSON of the last layout that went, or that came
    std::map<std::uint32_t, std::uint32_t> m_channels; ///< the state channel of each pane
    std::map<std::uint32_t, QPointer<Pane>> m_panes;
    /// The panes that the window binds next: those of the layout that came,
    /// and those that newPane started. A pane that the daemon opens and that
    /// is none of these, nor bound, nor released, is nobody's.
    std::set<std::uint32_t> m_unbound;
    /// The panes that the window let go while the session runs: their frames
    /// are read and dropped until their channel closes, which frees its ID.
    std::set<std::uint32_t> m_released;
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
        std::uint32_t pane = 0; ///< whose input it is: 0 for the layout
    };
    /// One queue for the control channel: the daemon reads it in order. At
    /// most one layout waits, the newest.
    std::deque<WaitingInput> m_waitingInput;
    std::deque<std::string> m_waitingActions; ///< ClosePane frames that wait for the action channel
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
