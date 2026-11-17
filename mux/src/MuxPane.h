// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ImageDecoder.h"
#include "MuxHost.h"
#include "MuxProtocol.h"

#include <AfterglowPty/Pty.h>
#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include <vector>

#include <sys/types.h>

namespace AfterglowMux {

/// One pane of a session: the program's PTY and the terminal that parses its
/// output. The daemon parses whether or not a window shows the session. The
/// terminal keeps the title and the working directory. While a window shows
/// the pane, the calls of the terminal to its host (MuxHost.h) wait for that
/// window: bells, notifications, the clipboard, transfers, drags, downloads,
/// and the reports that answer queries (DA1, DSR, colors, sizes). Without a
/// window, the pane answers them as a terminal with no user: a report goes to
/// the program, a request gets a denial, the rest go. Only a bell, a flash
/// and the newest notifications wait for the next window
/// (setKeptNotifications()). The query of the socket
/// of an ssh session gets its answer here always. PNG and iTerm2 images
/// decode in a process of their own (ImageDecoder), also while no window
/// shows the pane: the store keeps their pixels. Sixel and iTerm2 images
/// need a cell size, which only a window measures: before the first window,
/// they do not show.
///
/// The callbacks of the terminal and of the PTY run on the PTY reader thread,
/// with m_mutex held for the terminal's, and the terminal's also on the
/// daemon's thread for a resize, a reset, new terminal values (setOptions),
/// a clear and a visibility change.
/// They reach the daemon's thread only through @p wake, which must be safe to
/// call from any thread. While an image decodes, the reader thread lets
/// m_mutex go and the terminal stays its own: the daemon's thread does not
/// wait for it. A frame then waits, and a change waits for the end of the
/// feed, which makes it. Input after a change that waits waits behind it, in
/// the pane, up to half of the PTY's queue.
class MuxPane final : public AfterglowVt::ITerminalHost, public AfterglowPty::IPtyListener
{
public:
    /// @p wake runs when the program exits, after output, a resize, a reset,
    /// new terminal values, a clear or a host call once takeChanged() took
    /// the change before it, when a host call waits for the window and none
    /// waited before, when a watch event waits for the daemon and none waited
    /// before (setWatched()), when the PTY's input queue has room again after
    /// it refused input, or takes no more input, and at the end of a feed that
    /// made the changes and wrote the input that waited for its decode. The
    /// reader thread runs it for output, host calls, watch events, the exit,
    /// the queue and that feed. The daemon's thread runs it for the others,
    /// and for the host calls of a resize, a reset, a visibility change and a
    /// new window.
    /// It must be safe to call from any thread.
    /// @p images decodes the pane's PNG and iTerm2 images, and outlives it.
    MuxPane(std::uint32_t id, const AfterglowVt::TerminalOptions& options, const PaneSize& size,
            std::string notificationCapabilities, ImageDecoder& images, std::function<void()> wake);
    /// Hangs up the program: the PTY's reaper kills it when it stays.
    ~MuxPane() override;
    MuxPane(const MuxPane&) = delete;
    MuxPane& operator=(const MuxPane&) = delete;

    std::uint32_t id() const { return m_id; }
    bool start(AfterglowPty::SpawnOptions options, std::string launchedProgram, std::string* error);
    /// The wait status of the program, once exited().
    int exitStatus() const { return m_exitStatus.load(); }
    /// Whether something for the next frame came since the last call: output,
    /// a host call, a change of the daemon (a resize, a reset, new terminal
    /// values, a clear), or a frame that waited for a decode. No call, as
    /// while no window is attached, wakes the daemon no more: the next
    /// window's first frame carries everything.
    bool takeChanged() { return m_changed.exchange(false); }
    /// What input() did.
    enum class Input : std::uint8_t {
        Written,
        /// the PTY's queue is full, or the input that waits behind a change
        /// for a decode reached its bound: the wake says when it has room
        /// again (also the wake at the end of that feed), or takes none
        /// (then Gone)
        Full,
        Gone, ///< the program exited, or its terminal takes no more input
    };
    /// Input of an attached window for the program.
    Input input(std::string_view bytes);
    /// The size that an attached window measured.
    void resize(const PaneSize& size);
    /// The programs' TMPDIR (Terminal::setTemporaryDirectory). Before start().
    void setTemporaryDirectory(const std::string& directory)
    {
        std::lock_guard lock(m_mutex);
        m_terminal.setTemporaryDirectory(directory);
    }
    /// What the window's Reset and Clear actions do to a terminal of its
    /// own: the window shows the result with the next frame.
    void reset();
    void clear();
    /// The window's answer to a DownloadBegin call, or false to stop a
    /// download that streams: the pane's output goes on.
    void answerDownload(bool accept);
    /// New terminal values (Terminal::setOptions), when they differ: the
    /// window shows the result with the next frame.
    void setOptions(const AfterglowVt::TerminalOptions& options);
    /// The program exited, or start() failed.
    bool exited() const { return m_exited.load(); }
    /// The next frame of @p writer (StateWriter::update), with the region
    /// scrolls since the frame before it. None while an image decodes: the
    /// pane is changed again, and @p wake runs when the decode's feed ends.
    /// Without @p screens, for a pane that its window does not show, the
    /// frame leaves the screens and the scrolls for a later one
    /// (StateWriter::updateHeld), and the host calls wait for that one: a
    /// call follows the text before it.
    std::optional<std::string> frame(AfterglowVt::StateWriter& writer, std::size_t maxBytes, bool screens = true);
    /// Whether the user can see the pane in its window: mode 2033 reports a
    /// change to the program. A pane that no window shows cannot be seen, and
    /// a change of window (setWindow) makes it unseen until the new window
    /// says otherwise.
    /// @p window: the Attachment::serial() of the window that says so; only
    /// the pane's window (setWindow) can make it seen.
    void setVisible(bool visible, std::uint64_t window);

    /// The program in the foreground, when it is not the shell: the rule of
    /// a normal window's confirmClose. Empty when @p caller leads the
    /// foreground process group (a client that runs in this pane, such as
    /// `afterglow mux kill` typed at its prompt): the caller is no job to ask
    /// about. 0 for no caller.
    std::string foregroundJob(pid_t caller) const;
    /// The answer to the query of `aglow run` for the socket of the ssh
    /// session that runs in the pane (OSC 24569): the daemon keeps that
    /// socket. Safe from any thread: the reader thread reads it.
    void setRemoteSocket(AfterglowProto::Remote::Answer answer)
    {
        std::lock_guard lock(m_remoteMutex);
        m_remoteSocket = std::move(answer);
    }
    /// Whether the pane's program, which runs while the pane does, leads
    /// terminal session @p session: the processes of that session run in
    /// this pane.
    bool leads(pid_t session) const;
    /// The working directory of the program in the foreground, else of the
    /// first program: Pty::currentWorkingDirectory, which reads only /proc.
    /// Empty once the program exited, when neither directory remains, and
    /// for a path that is not UTF-8.
    std::string directory() const;

    /// What `afterglow ctl ls` says of the pane.
    struct Facts
    {
        std::string title;
        /// The directory of this machine that the shell reported, else
        /// directory(). Not one that is no directory here, nor one with a
        /// control byte or that is not UTF-8, nor a plain path from a shell
        /// on another host (an ssh context).
        std::string directory;
        std::string command; ///< the running command (OSC 133), empty at the prompt
        PaneSize size;
        /// The cursor is in a prompt or its input (OSC 133 A or B, before C).
        bool atPrompt = false;
        /// The values of OSC 1337 SetUserVar.
        std::map<std::string, std::string> userVars;
    };
    /// Without the terminal, also while an image decodes: the reader thread
    /// keeps them, and checks a directory that the shell reports once, with
    /// no lock that the daemon's thread waits for.
    Facts facts() const;
    /// The leader of the foreground process group: the shell at its prompt.
    /// Nothing once the program exited. It reads /proc.
    std::optional<AfterglowPty::ForegroundProcess> foreground() const;
    /// The state and text of the newest program status record (OSC 7501),
    /// as the control target names them (idle, working, done, blocked,
    /// error): idle and empty without one. Nothing while an image decodes.
    std::optional<std::pair<std::string, std::string>> status() const;
    /// The title of the terminal, also while an image decodes.
    std::string title() const;

    /// What the terminal tells its host that `afterglow ctl watch` hears of
    /// the pane, while the daemon watches it (setWatched): the daemon need
    /// not look at the panes to find what changed.
    struct WatchEvent
    {
        enum class Kind { Bell, UserVar, CommandStarted, CommandFinished, Title, Status };
        Kind kind = Kind::Bell;
        std::string name{}; ///< UserVar: its name. Status: the state.
        std::string text{}; ///< UserVar: its value. A command, a title, a status: its text.
        int exitCode = 0; ///< CommandFinished
        std::uint64_t milliseconds = 0; ///< CommandFinished: how long it ran
    };
    /// The most events, and bytes of them, that wait for the daemon: past
    /// them, a new title, status or value of a user variable takes the place
    /// of the last one of its kind that waits, and the other new events go.
    static constexpr std::size_t MaxWatchEvents = 1024;
    static constexpr std::size_t MaxWatchBytes = std::size_t(4) << 20;
    void setWatched(bool watched);
    /// The events since the last take, oldest first.
    std::vector<WatchEvent> takeWatchEvents();
    /// The text of @p scope, for `afterglow ctl get-text`. Nothing while an
    /// image decodes.
    std::optional<std::string> text(AfterglowVt::Terminal::TextScope scope, const AfterglowVt::TextFormat& format = {}) const;
    /// The most bytes of text() and firstOutputOnScreen(), as the
    /// terminal reads by default and a window's pane reads.
    static constexpr std::size_t MaxText = 1024 * 1024;
    /// The first output with a row on the screen (kitty's
    /// first_cmd_output_on_screen), empty for none. Nothing while an image
    /// decodes, as text().
    std::optional<std::string> firstOutputOnScreen(const AfterglowVt::TextFormat& format = {}) const;
    /// The terminal's modes, which say how a key is sent: nothing while an
    /// image decodes, as text().
    std::optional<AfterglowVt::Modes> modes() const;
    /// The colours of the terminal by name (Terminal::namedColors). Nothing
    /// while an image decodes.
    std::optional<std::vector<AfterglowVt::Terminal::NamedColor>> colors() const;
    /// Applies the colour OSCs @p payloads as the program's own, and the
    /// next frame takes them to the window. False while an image decodes,
    /// and for a payload that is no colour, which goes with those after it.
    bool applyColors(const std::vector<std::string>& payloads);
    /// Sets a user variable as the program would (`afterglow ctl set-user-var`).
    void setUserVar(const std::string& name, const std::string& value) { userVarChanged(name, value); }
    /// Sends @p signal to the foreground process group of the program.
    bool signalProgram(int signal) const { return m_pty && !m_exited && m_pty->signalForeground(signal); }

    /// The window that shows the pane, by Attachment::serial(), or 0 for
    /// none. The calls of its terminal to its host (MuxHost.h) wait for that
    /// window, up to @p queueBytes with their Host::CallCost: Mux.eventQueue.
    /// A call that finds nothing else waiting goes up to Host::MaxCall,
    /// whatever the queue holds. A new
    /// window gets the pointer shape, the user variables and the running
    /// command again, and what the pane kept while no window showed it
    /// (setKeptNotifications()). The calls that wait for a window that goes
    /// get answerWithoutWindow().
    void setWindow(std::uint64_t window, std::size_t queueBytes);
    /// The calls that wait for @p window, encoded, oldest first: those pushed
    /// before the last frame() that left no rows of the screen pending, as
    /// many as fit @p budget bytes with their Host::CallCost, and at least
    /// one of them. None while the pane waits for another window.
    std::vector<std::string> takeHostCalls(std::uint64_t window, std::size_t budget);
    /// Calls that takeHostCalls() takes wait for @p window. Those of another
    /// window, which the pane's program left as it exited, never go: they do
    /// not count.
    bool hasHostCalls(std::uint64_t window) const;
    /// Calls wait for @p window, also those that wait for a frame.
    bool callsWaiting(std::uint64_t window) const;
    /// Answers @p encoded, a call that its window did not get, as a pane that
    /// no window shows: a request gets its denial, a report goes to the
    /// program, the others go. A bell, a flash and a notification wait for
    /// the window that the pane has now, else for the next one
    /// (setKeptNotifications()).
    void answerWithoutWindow(std::string_view encoded);
    /// The most bytes of the notifications that the pane keeps while no
    /// window shows it, encoded: one over it keeps no icon, and one that
    /// is still over it goes. A quarter of the smallest Mux.eventQueue: the
    /// next window takes them in its queue (setWindow()), and the answers
    /// of the program's queries still fit there.
    static constexpr std::size_t MaxKeptBytes = std::size_t(MinEventQueue) / 4;
    /// Mux.keptNotifications: the most notifications (OSC 9, OSC 777 and
    /// OSC 99) that the pane keeps while no window shows it, 0 for none,
    /// within MaxKeptBytes. The oldest go first. An OSC 99 notification
    /// with the id of a kept one replaces it, and a close of that id takes
    /// it away, as on a screen. The pane also keeps whether a bell and a
    /// flash rang. The next window (setWindow()) gets the bell, the flash
    /// and then the notifications in their order, marked missed
    /// (Host::Call::missed), and so do the calls of these kinds that a
    /// window that went did not get. Clipboard writes are not kept: a late
    /// one would replace what the user copied since.
    void setKeptNotifications(std::size_t count);
    /// The answer to OSC 99's query, the window's (SessionSettings): empty
    /// while the window shows no notifications, so that the query gets none.
    void setNotificationCapabilities(std::string capabilities);

    // ITerminalHost
    void writeToPty(std::string_view bytes) override;
    void desktopNotification(const AfterglowVt::NotificationRequest& request) override;
    std::string notificationCapabilities() override;
    bool clipboardRequest(const AfterglowVt::ClipboardRequest& request) override;
    void terminalProtocol(int code, std::string_view payload) override;
    void bell() override;
    void flash() override;
    void titleChanged(std::string_view title) override;
    void workingDirectoryChanged(std::string_view directory) override;
    void contextChanged() override;
    void programStatusChanged() override;
    void notify(std::string_view title, std::string_view body) override;
    void clipboardWrite(std::string_view selection, std::string_view data) override;
    void pointerShapeChanged(std::string_view shape) override;
    void userVarChanged(std::string_view name, std::string_view value) override;
    void commandStarted(std::string_view command) override;
    void commandFinished(int exitCode) override;
    bool downloadBegin(std::string_view name, std::int64_t size) override;
    bool downloadData(std::string_view bytes) override;
    void downloadEnd(bool complete) override;
    void regionScrolled(const AfterglowVt::RegionScroll& scroll) override;
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override;
    bool decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height,
                     std::vector<std::uint8_t>& rgba) override;
    bool decodeImageFrames(std::string_view data, std::size_t maxBytes, std::vector<AfterglowVt::ImageFrame>& frames,
                           std::vector<std::uint32_t>& delays) override;
    /// The answer that the daemon gave the pane for its ssh sessions
    /// (setRemoteSocket): without one, `aglow run` hears that it is off, not
    /// that the terminal does not know the query.
    std::optional<AfterglowProto::Remote::Answer> remoteSocket() override
    {
        std::lock_guard lock(m_remoteMutex);
        return m_remoteSocket;
    }

    // IPtyListener
    void ptyData(std::string_view bytes) override;
    void ptyClosed(int status) override;
    void ptyWritable() override { m_wake(); }

private:
    /// Makes @p change to the terminal, m_mutex held, or leaves it to the
    /// reader thread while an image decodes. Daemon's thread.
    void change(std::function<void()> change);
    void setVisibleLocked(bool visible, std::uint64_t window);
    bool decodeOutsideLock(std::string_view data, ImageFormats formats, std::uint32_t& width, std::uint32_t& height,
                           std::vector<std::uint8_t>& rgba);
    /// ptyData(): waits while a download holds the output or fills the queue.
    void waitForDownload(std::unique_lock<std::mutex>& lock);
    /// Runs @p decode on the reader thread with m_mutex let go, as a decode
    /// can take seconds.
    bool outsideLock(const std::function<bool()>& decode);
    static std::pair<std::string, std::string> statusOf(const AfterglowVt::ProgramStatus& status);
    /// Keeps @p event for the daemon, which watches (m_watched): true when
    /// the daemon must wake for it, as nothing waited before. m_hostMutex
    /// held.
    bool watchLocked(WatchEvent event);
    /// The size of the terminal now, for facts(). m_mutex held.
    void keepSizeLocked();

    const std::uint32_t m_id;
    std::function<void()> m_wake;
    std::atomic<bool> m_changed{false};
    std::atomic<int> m_exitStatus{0};
    /// ptyData() waits on it, m_mutex held, while the terminal holds the
    /// output for a download's answer, or while the download's data fills
    /// the window's queue.
    std::condition_variable m_downloadWake;
    std::atomic<bool> m_closing{false};
    std::string m_notificationCapabilities; ///< under m_hostMutex
    ImageDecoder& m_images;
    /// Readable once the pane closes: a decode that runs, and any after it,
    /// ends at once.
    AfterglowProto::UniqueFd m_stopDecode;
    int m_stopDecodeError = 0; ///< errno of its eventfd(), for start()
    /// Guards m_terminal, which the reader thread feeds, and what follows it
    /// up to m_reportChanged.
    mutable std::mutex m_mutex;
    /// The lock of the feed that runs, which a decode lets go. Reader thread.
    std::unique_lock<std::mutex>* m_feedLock = nullptr;
    /// An image decodes: the terminal is the reader thread's, without m_mutex.
    bool m_decoding = false;
    /// The changes of the daemon's thread that wait for the feed's end, and
    /// the input that came after them.
    std::vector<std::function<void()>> m_deferred;
    /// m_deferred holds something, until its changes ran: input waits.
    std::atomic<bool> m_changesWaiting{false};
    /// The bytes of input in m_deferred.
    std::size_t m_deferredInput = 0;
    /// A frame waited for a decode: the end of the feed wakes the daemon.
    bool m_frameDeferred = false;
    AfterglowVt::Terminal m_terminal;
    /// The region scrolls of the terminal for the next frame.
    AfterglowVt::RegionScrolls m_scrolls;
    /// The shell reported a directory or a context in the feed: ptyData()
    /// checks the directory after it, outside m_mutex.
    bool m_reportChanged = false;
    /// After m_terminal: it goes first, and its reader thread ends before
    /// the terminal that the thread feeds.
    std::unique_ptr<AfterglowPty::Pty> m_pty;
    std::string m_launchedProgram;
    std::atomic<bool> m_exited{false};
    /// Terminal::atShellPrompt after the last feed: facts() takes no lock
    /// that the reader thread holds while an image decodes.
    std::atomic<bool> m_atPrompt{false};

    /// A call that waits for the window. PointerShape, UserVar and
    /// ProtocolLost wait empty, and take their newest value when they go.
    struct WaitingCall
    {
        Host::Kind kind = Host::Kind::Bell;
        std::string encoded;
        std::string name; ///< UserVar
        std::size_t cost = 0; ///< what it counts in m_hostBytes
        std::uint64_t number = 0; ///< its place among the pushes, from 1
    };
    /// Whether a call that does not fit Mux.eventQueue waits all the same:
    /// the end of a start that went, as the terminal of a window never drops
    /// it. Bells, flashes, the pointer shape, user variables and ProtocolLost
    /// pass the limit too, through pushLocked(): one of each kind waits at
    /// most, and one for each name of a user variable (MaxUserVars). So does
    /// the CommandStarted that a new window gets (setWindow()), and the
    /// notifications that the pane kept for it (setKeptNotifications()).
    enum class Fit : std::uint8_t { Queue, Always };
    enum class Forward : std::uint8_t {
        Queued,
        NoWindow, ///< the caller answers as a pane that no window shows
        NoRoom, ///< the call does not fit: as without a window, or dropped
    };
    /// Queues @p call for the window as it fits, decided under one lock.
    Forward forward(const Host::Call& call);
    /// m_hostMutex held. True when it is the first call that waits: the
    /// caller wakes the daemon, after the lock.
    bool pushLocked(WaitingCall call);
    /// forward() with m_hostMutex held, for a call encoded already. @p wake
    /// as pushLocked() returns it.
    bool queueLocked(std::string encoded, Host::Kind kind, Fit fit, bool* wake);
    /// The bytes of @p waiting as it goes now. m_hostMutex held.
    std::string encodeLocked(const WaitingCall& waiting) const;
    /// Bytes for the program, past the window.
    void writeDirect(std::string_view bytes);
    /// Writes the answer of a pane that no window shows to @p call
    /// (Host::answerWithoutWindow), if it has one.
    void answerLocally(const Host::Call& call);
    /// answerWithoutWindow() without the keep: m_hostMutex may be held.
    void answerUnsent(std::string_view encoded);
    /// What answerWithoutWindow() keeps of @p encoded. Without m_hostMutex.
    void keepUnsent(std::string_view encoded);
    /// Keeps @p call, a notification or an OSC 99 close that forward() found
    /// no window for, or that a window did not get (keepUnsent())
    /// (setKeptNotifications()). A window that came since forward() gets it
    /// as a call that waits for it.
    void keep(const Host::Call& call);
    /// @p call as the next window gets it: a notification marked missed,
    /// without its icon when it would not fit MaxKeptBytes.
    static std::string keptEncoding(const Host::Call& call);
    /// keep() with m_hostMutex held, for @p encoded (keptEncoding()).
    void keepLocked(const Host::Call& call, std::string encoded);
    /// Keeps what @p waiting, a call that its window did not get, holds for
    /// the next window: a bell, a flash or a notification. m_hostMutex held.
    void keepWaitingLocked(const WaitingCall& waiting);

    /// Guards m_remoteSocket alone: the reader thread asks for it while it
    /// holds m_mutex.
    std::mutex m_remoteMutex;
    AfterglowProto::Remote::Answer m_remoteSocket;

    /// Guards what follows: the reader thread queues host calls and watch
    /// events, and keeps what facts() says, and the daemon's thread takes
    /// them.
    mutable std::mutex m_hostMutex;
    bool m_watched = false;
    std::vector<WatchEvent> m_watchEvents;
    std::size_t m_watchBytes = 0; ///< of m_watchEvents
    /// The terminal's title and size, for facts() while an image decodes.
    std::string m_title;
    PaneSize m_size;
    /// The local directory that the shell last reported, checked: empty for
    /// none (localDirectory()).
    std::string m_checkedDirectory;
    std::uint64_t m_window = 0;
    /// The window that a download asked: its data and end go to it only. A
    /// window that goes, or another one, ends the download.
    std::uint64_t m_downloadWindow = 0;
    std::size_t m_hostLimit = 0;
    std::deque<WaitingCall> m_hostCalls;
    std::size_t m_hostBytes = 0;
    /// The calls pushed so far, and those pushed before the last frame whose
    /// screen was whole: takeHostCalls() takes only these.
    std::uint64_t m_pushed = 0;
    std::uint64_t m_framed = 0;
    bool m_bellWaiting = false;
    bool m_flashWaiting = false;
    bool m_pointerWaiting = false;
    std::set<std::string> m_userVarsWaiting;
    /// Kept while no window shows the pane: the next window gets them.
    std::string m_pointerShape;
    std::map<std::string, std::string> m_userVars;
    std::string m_command;
    bool m_commandRunning = false;
    /// The window got the start of the running command: it gets the end.
    bool m_commandForwarded = false;
    std::chrono::steady_clock::time_point m_commandStart;
    /// While a ProtocolLost call waits, the protocol commands go into it:
    /// what they held for drag and drop (Host::LostNothing...), and whether
    /// one was a reset.
    bool m_protocolLostWaiting = false;
    int m_protocolLost = Host::LostNothing;
    bool m_protocolLostReset = false;
    /// What waits for the next window while none shows the pane
    /// (setKeptNotifications()): the notifications, encoded, oldest first.
    struct KeptCall
    {
        Host::Kind kind = Host::Kind::Notify;
        std::string encoded;
        std::string id; ///< of an OSC 99 notification, which a later one replaces
    };
    std::size_t m_keptLimit = std::size_t(DefaultKeptNotifications);
    std::deque<KeptCall> m_kept;
    std::size_t m_keptBytes = 0; ///< of m_kept
    bool m_bellKept = false;
    bool m_flashKept = false;
};

} // namespace AfterglowMux
