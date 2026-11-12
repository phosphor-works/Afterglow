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
/// the program, a request gets a denial, the rest go. The query of the socket
/// of an ssh session gets its answer here always. PNG and iTerm2 images
/// decode in a process of their own (ImageDecoder), also while no window
/// shows the pane: the store keeps their pixels. Sixel and iTerm2 images
/// need a cell size, which only a window measures: before the first window,
/// they do not show.
///
/// The callbacks of the terminal and of the PTY run on the PTY reader thread,
/// with m_mutex held for the terminal's, and the terminal's also on the
/// daemon's thread for a resize, a reset, a clear and a visibility change.
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
    /// a clear or a host call once takeChanged() took the change before it,
    /// when a host call waits for the window and none waited before, when the
    /// PTY's input queue has room again after it refused input, or takes no
    /// more input, and at the end of a feed that made the changes and wrote
    /// the input that waited for its decode. The reader thread runs it for
    /// output, host calls, the exit, the queue and that feed. The daemon's
    /// thread runs it for the others, and for the host calls of a resize, a
    /// reset, a visibility change and a new window.
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
    /// Whether output or a host call came since the last call. No call, as while no window
    /// is attached, wakes the daemon no more: the next window's first frame
    /// carries everything.
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
    /// The program exited, or start() failed.
    bool exited() const { return m_exited.load(); }
    /// The next frame of @p writer (StateWriter::update), with the region
    /// scrolls since the frame before it. None while an image decodes: the
    /// pane is changed again, and @p wake runs when the decode's feed ends.
    std::optional<std::string> frame(AfterglowVt::StateWriter& writer, std::size_t maxBytes);
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
    /// The working directory of the program in the foreground, else of the
    /// first program: Pty::currentWorkingDirectory, which reads only /proc.
    /// Empty once the program exited, when neither directory remains, and
    /// for a path that is not UTF-8.
    std::string directory() const;

    /// The window that shows the pane, by Attachment::serial(), or 0 for
    /// none. The calls of its terminal to its host (MuxHost.h) wait for that
    /// window, up to @p queueBytes with their Host::CallCost: Mux.eventQueue.
    /// A call that finds nothing else waiting goes whatever its size. A new
    /// window gets the pointer shape, the user variables and the running
    /// command again. The calls that wait for a window that goes get
    /// answerWithoutWindow().
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
    /// Answers @p encoded, a call that its window did not get, as a pane that
    /// no window shows: a request gets its denial, a report goes to the
    /// program, the others go.
    void answerWithoutWindow(std::string_view encoded);

    // ITerminalHost
    void writeToPty(std::string_view bytes) override;
    void desktopNotification(const AfterglowVt::NotificationRequest& request) override;
    std::string notificationCapabilities() override { return m_notificationCapabilities; }
    bool clipboardRequest(const AfterglowVt::ClipboardRequest& request) override;
    void terminalProtocol(int code, std::string_view payload) override;
    void bell() override;
    void flash() override;
    void notify(std::string_view title, std::string_view body) override;
    void clipboardWrite(std::string_view selection, std::string_view data) override;
    void pointerShapeChanged(std::string_view shape) override;
    void userVarChanged(std::string_view name, std::string_view value) override;
    void commandStarted(std::string_view command) override;
    void commandFinished(int exitCode) override;
    void downloadFile(std::string_view name, std::string_view data) override;
    void regionScrolled(const AfterglowVt::RegionScroll& scroll) override;
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override;
    bool decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height,
                     std::vector<std::uint8_t>& rgba) override;
    /// A session pane forwards no socket of an ssh session (afterglow ssh
    /// runs in windows only): `aglow run` hears that it is off, not that the
    /// terminal does not know the query.
    std::optional<AfterglowProto::Remote::Answer> remoteSocket() override { return AfterglowProto::Remote::Answer{}; }

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

    const std::uint32_t m_id;
    std::function<void()> m_wake;
    std::atomic<bool> m_changed{false};
    std::atomic<int> m_exitStatus{0};
    const std::string m_notificationCapabilities;
    ImageDecoder& m_images;
    /// Readable once the pane closes: a decode that runs, and any after it,
    /// ends at once.
    AfterglowProto::UniqueFd m_stopDecode;
    int m_stopDecodeError = 0; ///< errno of its eventfd(), for start()
    mutable std::mutex m_mutex; ///< guards m_terminal, which the reader thread feeds, m_scrolls and what follows
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
    /// After m_terminal: it goes first, and its reader thread ends before
    /// the terminal that the thread feeds.
    std::unique_ptr<AfterglowPty::Pty> m_pty;
    std::string m_launchedProgram;
    std::atomic<bool> m_exited{false};

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
    /// pass the limit too, through pushLocked(): one of each waits at most.
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
    /// The answers of a pane that no window shows.
    void clipboardWithoutWindow(const AfterglowVt::ClipboardRequest& request);
    void protocolWithoutWindow(int code, std::string_view payload);
    void notificationWithoutWindow(const AfterglowVt::NotificationRequest& request);

    /// Guards what follows: the reader thread queues host calls, and the
    /// daemon's thread takes them.
    mutable std::mutex m_hostMutex;
    std::uint64_t m_window = 0;
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
};

} // namespace AfterglowMux
