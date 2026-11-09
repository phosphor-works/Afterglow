// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxProtocol.h"

#include <AfterglowPty/Pty.h>
#include <AfterglowVt/Terminal.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace AfterglowMux {

/// One pane of a session: the program's PTY and the terminal that parses its
/// output. The daemon parses whether or not a window shows the session, and
/// answers the queries that the terminal answers itself (DA1, DSR, colors,
/// sizes, the socket of an ssh session). The terminal keeps the title and the
/// working directory. The state that a window shows carries no bells,
/// notifications or clipboard yet (docs/MUX.md): they are dropped or refused
/// here, also while a window shows the session. Images that need a decoder
/// (PNG, iTerm2) are refused: Afterglow decodes them with Qt, which the
/// daemon does not have, and aglow runs stb_image only in a child process
/// with limits, which a process with PTY threads cannot fork safely.
///
/// The callbacks of the terminal and of the PTY run on the PTY reader thread,
/// with m_mutex held for the terminal's. They reach the daemon's thread only
/// through @p wake, which must be safe to call from any thread.
class MuxPane final : public AfterglowVt::ITerminalHost, public AfterglowPty::IPtyListener
{
public:
    /// @p wake runs when the program exits, after output, a resize, a reset
    /// or a clear while takeChanged() has not taken the last change yet, and
    /// when the PTY's input queue has room again after it refused input. The
    /// reader thread runs it for output, the exit and room, the daemon's
    /// thread for the others: it must be safe to call from any thread.
    MuxPane(std::uint32_t id, const AfterglowVt::TerminalOptions& options, const PaneSize& size,
            std::string notificationCapabilities, std::function<void()> wake);
    /// Hangs up the program: the PTY's reaper kills it when it stays.
    ~MuxPane() override;
    MuxPane(const MuxPane&) = delete;
    MuxPane& operator=(const MuxPane&) = delete;

    std::uint32_t id() const { return m_id; }
    bool start(AfterglowPty::SpawnOptions options, std::string launchedProgram, std::string* error);
    /// The wait status of the program, once exited().
    int exitStatus() const { return m_exitStatus.load(); }
    /// Whether output came since the last call. No call, as while no window
    /// is attached, wakes the daemon no more: the next window's first frame
    /// carries everything.
    bool takeChanged() { return m_changed.exchange(false); }
    /// What input() did.
    enum class Input : std::uint8_t {
        Written,
        Full, ///< the PTY's queue is full: the wake says when it has room again
        Gone, ///< the program exited
    };
    /// Input of an attached window for the program.
    Input input(std::string_view bytes);
    /// The size that an attached window measured.
    void resize(const PaneSize& size);
    /// What the window's Reset and Clear actions do to a terminal of its
    /// own: the window shows the result with the next frame.
    void reset();
    void clear();
    /// Runs @p fn with the terminal, under the lock that the reader thread
    /// takes to feed it.
    template<typename F>
    auto withTerminal(F&& fn) const
    {
        std::lock_guard lock(m_mutex);
        return fn(m_terminal);
    }
    /// The program exited, or start() failed.
    bool exited() const { return m_exited.load(); }

    /// The program in the foreground, when it is not the shell: the rule of
    /// a normal window's confirmClose. Empty when @p caller leads the
    /// foreground process group (a client that runs in this pane, such as
    /// `afterglow mux kill` typed at its prompt): the caller is no job to ask
    /// about. 0 for no caller.
    std::string foregroundJob(pid_t caller) const;

    // ITerminalHost
    void writeToPty(std::string_view bytes) override;
    void desktopNotification(const AfterglowVt::NotificationRequest& request) override;
    std::string notificationCapabilities() override { return m_notificationCapabilities; }
    bool clipboardRequest(const AfterglowVt::ClipboardRequest& request) override;
    void terminalProtocol(int code, std::string_view payload) override;
    /// A session pane forwards no socket of an ssh session (afterglow ssh
    /// runs in windows only): `aglow run` hears that it is off, not that the
    /// terminal does not know the query.
    std::optional<AfterglowProto::Remote::Answer> remoteSocket() override { return AfterglowProto::Remote::Answer{}; }

    // IPtyListener
    void ptyData(std::string_view bytes) override;
    void ptyClosed(int status) override;
    void ptyWritable() override { m_wake(); }

private:
    const std::uint32_t m_id;
    std::function<void()> m_wake;
    std::atomic<bool> m_changed{false};
    std::atomic<int> m_exitStatus{0};
    const std::string m_notificationCapabilities;
    mutable std::mutex m_mutex; ///< guards m_terminal, which the reader thread feeds
    AfterglowVt::Terminal m_terminal;
    /// After m_terminal: it goes first, and its reader thread ends before
    /// the terminal that the thread feeds.
    std::unique_ptr<AfterglowPty::Pty> m_pty;
    std::string m_launchedProgram;
    std::atomic<bool> m_exited{false};
};

} // namespace AfterglowMux
