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

namespace AfterglowMux {

/// One pane of a session: the program's PTY and the terminal that parses its
/// output. The daemon keeps parsing while no client is attached, and answers
/// the queries that the terminal answers itself (DA1, DSR, colors, sizes).
///
/// The callbacks of the terminal and of the PTY run on the PTY reader thread,
/// with m_mutex held for the terminal's. They reach the daemon's thread only
/// through @p exited, which must be safe to call from any thread.
class MuxPane final : public AfterglowVt::ITerminalHost, public AfterglowPty::IPtyListener
{
public:
    /// The events that wait for the next client while none is attached. The
    /// daemon keeps their count and drops their content.
    struct Missed
    {
        std::uint64_t bells = 0;
        std::uint64_t notifications = 0;
        std::uint64_t clipboardWrites = 0;
    };

    MuxPane(std::uint32_t id, const AfterglowVt::TerminalOptions& options, const PaneSize& size,
            std::function<void()> exited);
    ~MuxPane() override;
    MuxPane(const MuxPane&) = delete;
    MuxPane& operator=(const MuxPane&) = delete;

    std::uint32_t id() const { return m_id; }

    bool start(AfterglowPty::SpawnOptions options, std::string launchedProgram, std::string* error);
    /// Ends the program: the PTY closes, and the reader thread stops.
    void stop();
    /// The program exited, or start() failed.
    bool exited() const { return m_exited.load(); }

    /// The program in the foreground, when it is not the shell: the rule of
    /// a normal window's confirmClose.
    std::string foregroundJob() const;
    std::string title() const;
    Missed missed() const;

    // ITerminalHost
    void writeToPty(std::string_view bytes) override;
    void titleChanged(std::string_view title) override;
    void workingDirectoryChanged(std::string_view directory) override;
    void bell() override;
    void clipboardWrite(std::string_view selection, std::string_view data) override;
    void notify(std::string_view title, std::string_view body) override;
    void desktopNotification(const AfterglowVt::NotificationRequest& request) override;
    bool clipboardRequest(const AfterglowVt::ClipboardRequest& request) override;
    void terminalProtocol(int code, std::string_view payload) override;

    // IPtyListener
    void ptyData(std::string_view bytes) override;
    void ptyClosed(int status) override;

private:
    const std::uint32_t m_id;
    std::function<void()> m_exitedCallback;
    mutable std::mutex m_mutex; ///< guards m_terminal and the values below it
    AfterglowVt::Terminal m_terminal;
    std::string m_title;
    std::string m_workingDirectory;
    Missed m_missed;
    std::unique_ptr<AfterglowPty::Pty> m_pty;
    std::string m_launchedProgram;
    std::atomic<bool> m_exited{false};
};

} // namespace AfterglowMux
