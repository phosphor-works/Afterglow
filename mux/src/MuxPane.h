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
#include <string>
#include <string_view>

#include <sys/types.h>

namespace AfterglowMux {

/// One pane of a session: the program's PTY and the terminal that parses its
/// output. The daemon keeps parsing while no client is attached, and answers
/// the queries that the terminal answers itself (DA1, DSR, colors, sizes).
/// The terminal keeps the title and the working directory. Bells,
/// notifications and clipboard writes are dropped, as no user sees them.
/// Images that need a decoder (PNG, iTerm2) are refused: Afterglow decodes
/// them with Qt, which the daemon does not have, and aglow runs stb_image only
/// in a child process with limits, which a process with PTY threads cannot
/// fork safely.
///
/// The callbacks of the terminal and of the PTY run on the PTY reader thread,
/// with m_mutex held for the terminal's. They reach the daemon's thread only
/// through @p exited, which must be safe to call from any thread.
class MuxPane final : public AfterglowVt::ITerminalHost, public AfterglowPty::IPtyListener
{
public:
    MuxPane(const AfterglowVt::TerminalOptions& options, const PaneSize& size, std::string notificationCapabilities,
            std::function<void()> exited);
    /// Hangs up the program: the PTY's reaper kills it when it stays.
    ~MuxPane() override;
    MuxPane(const MuxPane&) = delete;
    MuxPane& operator=(const MuxPane&) = delete;

    bool start(AfterglowPty::SpawnOptions options, std::string launchedProgram, std::string* error);
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

    // IPtyListener
    void ptyData(std::string_view bytes) override;
    void ptyClosed(int status) override;

private:
    std::function<void()> m_exitedCallback;
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
